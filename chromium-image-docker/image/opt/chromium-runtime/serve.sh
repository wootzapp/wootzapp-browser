#!/usr/bin/env bash
exec 2>&1
set -euo pipefail

CHROME_BIN="${CHROMIUM_BIN:-/opt/wootzapp/chrome}"
RUNTIME_USER="${CHROMIUM_RUNTIME_USER:-wootz}"
RUNTIME_HOME="${CHROMIUM_HOME:-/home/wootz}"
PROFILE_DIR="${CHROMIUM_USER_DATA_DIR:-${RUNTIME_HOME}/wootz-browser-sessions}"
RUNTIME_DIR="${CHROMIUM_RUNTIME_DIR:-/tmp/chromium-runtime}"
INTERNAL_PORT="${CHROMIUM_CDP_PORT:-9223}"
PUBLIC_HOST="${CDP_BIND_HOST:-0.0.0.0}"
PUBLIC_PORT="${CDP_PORT:-9222}"
START_URL="${CHROMIUM_START_URL:-about:blank}"
DISPLAY_VALUE="${DISPLAY:-:0}"
VNC_PORT_VALUE="${VNC_PORT:-5900}"
NOVNC_PORT_VALUE="${NOVNC_PORT:-6080}"
VNC_SCREEN_VALUE="${VNC_SCREEN:-1366x900x24}"
WINDOW_WIDTH="${CHROMIUM_WINDOW_WIDTH:-1366}"
WINDOW_HEIGHT="${CHROMIUM_WINDOW_HEIGHT:-900}"
AUTHORING_MODE="${AUTHORING_MODE:-off}"
AUTHORING_SESSION="${AUTHORING_SESSION:-default}"

CHROMIUM_PID=""
SOCAT_PID=""
XVFB_PID=""
VNC_PID=""
NOVNC_PID=""
HEADLESS=""
RESET_PROFILE=""

log() {
  echo "[wootz-runtime] $*"
}

normalize_bool() {
  case "${1:-}" in
    1|true|TRUE|yes|YES|on|ON) printf '1' ;;
    *) printf '0' ;;
  esac
}

validate_runtime_options() {
  HEADLESS="$(normalize_bool "${CHROMIUM_HEADLESS:-1}")"
  RESET_PROFILE="$(normalize_bool "${CHROMIUM_RESET_PROFILE:-1}")"

  case "${AUTHORING_MODE}" in
    off|capture|replay) ;;
    *) log "invalid AUTHORING_MODE=${AUTHORING_MODE}; expected off, capture, or replay"; return 1 ;;
  esac

  case "${AUTHORING_SESSION}" in
    (*[!A-Za-z0-9._-]*|'') log "invalid AUTHORING_SESSION=${AUTHORING_SESSION}"; return 1 ;;
  esac
}

stop_pid() {
  local pid="$1"
  if [ -n "${pid}" ] && kill -0 "${pid}" 2>/dev/null; then
    kill "${pid}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true
  fi
}

cleanup() {
  stop_pid "${SOCAT_PID}"
  stop_pid "${NOVNC_PID}"
  stop_pid "${VNC_PID}"
  stop_pid "${CHROMIUM_PID}"
  stop_pid "${XVFB_PID}"
  rm -f "${RUNTIME_DIR}/cdp-ready"
}

wait_for_display() {
  local display_number="${DISPLAY_VALUE#:}"
  display_number="${display_number%%.*}"
  local display_socket="/tmp/.X11-unix/X${display_number}"
  for _ in $(seq 1 120); do
    if [ -S "${display_socket}" ]; then
      return 0
    fi
    sleep 0.5
  done
  return 1
}

wait_for_private_cdp() {
  for _ in $(seq 1 120); do
    if CDP_PROBE_HOST=127.0.0.1 CDP_PORT="${INTERNAL_PORT}" python3 /opt/chromium-runtime/status.py --quiet --timeout 2 >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.5
  done
  return 1
}

wait_for_public_cdp() {
  for _ in $(seq 1 60); do
    if CDP_PROBE_HOST=127.0.0.1 CDP_PORT="${PUBLIC_PORT}" python3 /opt/chromium-runtime/status.py --quiet --timeout 2 >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.5
  done
  return 1
}

start_display_stack() {
  if [ "${HEADLESS}" = "1" ]; then
    log "headless mode; no display/VNC/noVNC services."
    return 0
  fi

  mkdir -p /tmp/.X11-unix
  chmod 1777 /tmp/.X11-unix
  rm -f "/tmp/.X${DISPLAY_VALUE#:}-lock"

  log "starting Xvfb display=${DISPLAY_VALUE} screen=${VNC_SCREEN_VALUE}"
  Xvfb "${DISPLAY_VALUE}" -screen 0 "${VNC_SCREEN_VALUE}" -nolisten tcp &
  XVFB_PID="$!"
  if ! wait_for_display; then
    log "display did not become ready"
    return 1
  fi

  log "starting x11vnc port=${VNC_PORT_VALUE}"
  x11vnc -display "${DISPLAY_VALUE}" -forever -shared -nopw -rfbport "${VNC_PORT_VALUE}" &
  VNC_PID="$!"

  log "starting noVNC port=${NOVNC_PORT_VALUE}"
  if [ -x /usr/share/novnc/utils/novnc_proxy ]; then
    /usr/share/novnc/utils/novnc_proxy --vnc "127.0.0.1:${VNC_PORT_VALUE}" --listen "${NOVNC_PORT_VALUE}" &
  else
    websockify --web=/usr/share/novnc "${NOVNC_PORT_VALUE}" "127.0.0.1:${VNC_PORT_VALUE}" &
  fi
  NOVNC_PID="$!"
}

prepare_profile() {
  if [ ! -x "${CHROME_BIN}" ]; then
    log "missing executable: ${CHROME_BIN}"
    return 1
  fi

  if ! id -u "${RUNTIME_USER}" >/dev/null 2>&1; then
    log "missing runtime user: ${RUNTIME_USER}"
    return 1
  fi

  mkdir -p "${PROFILE_DIR}" "${RUNTIME_DIR}" "${RUNTIME_HOME}"
  chown -R "${RUNTIME_USER}:${RUNTIME_USER}" "${PROFILE_DIR}" "${RUNTIME_DIR}" "${RUNTIME_HOME}"

  if [ "${RESET_PROFILE}" = "1" ]; then
    rm -rf "${PROFILE_DIR:?}/"* "${PROFILE_DIR:?}/".[!.]* "${PROFILE_DIR:?}/"..?* 2>/dev/null || true
  fi

  rm -f "${PROFILE_DIR}/SingletonLock" "${PROFILE_DIR}/SingletonSocket" "${PROFILE_DIR}/SingletonCookie" 2>/dev/null || true
}

append_extra_args() {
  local raw_args="$1"
  if [ -n "${raw_args}" ]; then
    # shellcheck disable=SC2206
    local parsed_args=(${raw_args})
    args+=("${parsed_args[@]}")
  fi
}

start_chromium() {
  prepare_profile || return 1

  args=(
    "--remote-debugging-address=127.0.0.1"
    "--remote-debugging-port=${INTERNAL_PORT}"
    "--remote-allow-origins=*"
    "--user-data-dir=${PROFILE_DIR}"
    "--no-first-run"
    "--no-default-browser-check"
    "--disable-session-crashed-bubble"
    "--disable-background-networking"
    "--disable-sync"
    "--disable-component-update"
    "--enable-logging"
    "--log-file=${RUNTIME_DIR}/chrome.log"
    "--window-size=${WINDOW_WIDTH},${WINDOW_HEIGHT}"
  )

  if [ "${CHROMIUM_NO_SANDBOX:-1}" = "1" ]; then
    args+=("--no-sandbox")
  fi

  if [ "${HEADLESS}" = "1" ]; then
    args+=("--headless=new" "--disable-gpu")
  else
    args+=("--ozone-platform=x11" "--disable-gpu" "--disable-accelerated-2d-canvas")
  fi

  case "${AUTHORING_MODE}" in
    off)
      ;;
    capture)
      args+=("--wootz-authoring-capture" "--wootz-authoring-session=${AUTHORING_SESSION}")
      ;;
    replay)
      args+=("--wootz-authoring-replay" "--wootz-authoring-session=${AUTHORING_SESSION}")
      args+=("--host-resolver-rules=MAP * 0.0.0.0")
      ;;
  esac

  append_extra_args "${CHROMIUM_EXTRA_ARGS:-}"
  args+=("${START_URL}")

  {
    echo "headless=${HEADLESS}"
    echo "authoring_mode=${AUTHORING_MODE}"
    echo "authoring_session=${AUTHORING_SESSION}"
    echo "reset_profile=${RESET_PROFILE}"
    echo "browser_port=${INTERNAL_PORT}"
    echo "user_data_dir=${PROFILE_DIR}"
    echo "url=${START_URL}"
  } > "${RUNTIME_DIR}/browser-launch.env"

  log "starting w8-core authoring_mode=${AUTHORING_MODE} session=${AUTHORING_SESSION} headless=${HEADLESS} private_cdp=127.0.0.1:${INTERNAL_PORT}"
  runuser -u "${RUNTIME_USER}" -- env \
    HOME="${RUNTIME_HOME}" \
    DISPLAY="${DISPLAY_VALUE}" \
    CHROME_DEVEL_SANDBOX="${CHROME_DEVEL_SANDBOX:-/opt/wootzapp/chrome_sandbox}" \
    GOOGLE_API_KEY="${GOOGLE_API_KEY:-wootz-development-api-key}" \
    GOOGLE_DEFAULT_CLIENT_ID="${GOOGLE_DEFAULT_CLIENT_ID:-wootz-development-client-id}" \
    GOOGLE_DEFAULT_CLIENT_SECRET="${GOOGLE_DEFAULT_CLIENT_SECRET:-wootz-development-client-secret}" \
    AUTHORING_MODE="${AUTHORING_MODE}" \
    AUTHORING_SESSION="${AUTHORING_SESSION}" \
    "${CHROME_BIN}" "${args[@]}" &
  CHROMIUM_PID="$!"
}

start_cdp_bridge() {
  stop_pid "${SOCAT_PID}"
  rm -f "${RUNTIME_DIR}/cdp-ready"
  log "starting CDP bridge ${PUBLIC_HOST}:${PUBLIC_PORT} -> 127.0.0.1:${INTERNAL_PORT}"
  socat "TCP-LISTEN:${PUBLIC_PORT},bind=${PUBLIC_HOST},reuseaddr,fork" "TCP:127.0.0.1:${INTERNAL_PORT}" &
  SOCAT_PID="$!"
  if ! wait_for_public_cdp; then
    log "public CDP did not become ready"
    return 1
  fi
  touch "${RUNTIME_DIR}/cdp-ready"
}

trap 'cleanup; exit 0' TERM INT

validate_runtime_options
start_display_stack
start_chromium
if ! wait_for_private_cdp; then
  log "private CDP did not become ready"
  cleanup
  exit 1
fi
start_cdp_bridge

log "ready: CDP=http://127.0.0.1:${PUBLIC_PORT}/json/version"
if [ "${HEADLESS}" != "1" ]; then
  log "ready: noVNC=http://127.0.0.1:${NOVNC_PORT_VALUE}/vnc.html?host=127.0.0.1&port=${NOVNC_PORT_VALUE}&autoconnect=1&resize=scale"
fi

while kill -0 "${CHROMIUM_PID}" 2>/dev/null; do
  if [ -n "${SOCAT_PID}" ] && ! kill -0 "${SOCAT_PID}" 2>/dev/null; then
    log "CDP bridge exited; restarting"
    start_cdp_bridge || true
  fi
  sleep 2
done

wait "${CHROMIUM_PID}" 2>/dev/null || exit_code="$?"
cleanup
exit "${exit_code:-1}"
