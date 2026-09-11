#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
IMAGE_NAME="${CHROMIUM_IMAGE_NAME:-w8-core:chromium-desktop}"
CONTAINER_NAME="${CHROMIUM_CONTAINER_NAME:-w8-core-browser-engine}"
CDP_HOST_PORT="${CDP_PORT:-9222}"
VNC_HOST_PORT="${VNC_PORT:-5900}"
NOVNC_HOST_PORT="${NOVNC_PORT:-6080}"

ENV_ARGS=(
  -e CHROMIUM_HEADLESS="${CHROMIUM_HEADLESS:-1}"
)

for name in CHROMIUM_RESET_PROFILE AUTHORING_MODE AUTHORING_SESSION VNC_SCREEN CHROMIUM_START_URL CHROMIUM_EXTRA_ARGS; do
  if [ -n "${!name+x}" ]; then
    ENV_ARGS+=(-e "${name}=${!name}")
  fi
done

if docker ps --format '{{.Names}}' | grep -Fxq "${CONTAINER_NAME}"; then
  echo "${CONTAINER_NAME} is already running"
else
  docker run -d --rm \
    --name "${CONTAINER_NAME}" \
    --shm-size="${CHROMIUM_SHM_SIZE:-2g}" \
    -p "127.0.0.1:${CDP_HOST_PORT}:9222" \
    -p "127.0.0.1:${VNC_HOST_PORT}:5900" \
    -p "127.0.0.1:${NOVNC_HOST_PORT}:6080" \
    "${ENV_ARGS[@]}" \
    "${IMAGE_NAME}"
fi

docker exec "${CONTAINER_NAME}" chromium-runtime-entrypoint wait --timeout "${CHROMIUM_WAIT_TIMEOUT:-30}"
docker exec "${CONTAINER_NAME}" chromium-runtime-entrypoint status
echo "CDP: http://127.0.0.1:${CDP_HOST_PORT}/json/version"
if [ "${CHROMIUM_HEADLESS:-1}" != "1" ]; then
  echo "noVNC: http://127.0.0.1:${NOVNC_HOST_PORT}/vnc.html?host=127.0.0.1&port=${NOVNC_HOST_PORT}&autoconnect=1&resize=scale"
fi
