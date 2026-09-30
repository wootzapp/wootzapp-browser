# Authoring reference

Follow the [root README's capture and replay steps](../README.md#capture-and-replay-a-session) to record a session. This reference explains recording verification, stored data, and replay behavior.

Run commands from the directory containing `compose.yaml` and `.env`.

## Session configuration

`AUTHORING_MODE` accepts `off`, `capture`, or `replay`. `AUTHORING_SESSION` identifies the archive. Session names accept letters, numbers, dots, underscores, and dashes.

Capture and replay must use the same session name and profile volume, with `CHROMIUM_RESET_PROFILE=0`. Keep `CHROMIUM_START_URL` set to the recorded page when switching to replay.

## Verify the recording

Inspect the active mode, session name, and startup page:

```bash
docker compose exec w8-core cat /tmp/chromium-runtime/browser-launch.env
```

Find the recording indexes:

```bash
docker compose exec w8-core find /home/wootz/wootz-browser-sessions -name index.jsonl
```

Archives are stored under `WootzAuthoring/sessions/<session>` in the profile volume. `docker compose down` retains them; `docker compose down -v` removes the profile volume and its contents.

## Capture behavior

Capture records HTTP/HTTPS responses, redirects, response payloads, same-document SPA routes, localStorage for visited origins, and cookies for visited hosts. It bypasses service workers and HTTP cache for main document loading.

At capture startup, w8-core clears IndexedDB, CacheStorage, and service workers for the default storage partition. This cleanup does not clear cookies or localStorage.

## Replay behavior

Replay restores captured localStorage and cookies, serves archived responses and redirects, supports recorded SPA routes, and uses fallback matching for some document and static asset requests. It also provides a synthetic WebSocket connection for offline replay.

A strict replay miss returns a replay-miss response rather than fetching from the live network.

## Native browser flags

The Docker runtime translates `AUTHORING_MODE` and `AUTHORING_SESSION` into browser switches. If you run a prepared w8-core build directly, launch its executable (`chrome`) from the directory containing the browser payload.

Capture:

```bash
./chrome --wootz-authoring-capture \
  --wootz-authoring-session=my-session \
  --user-data-dir="$PWD/w8-core-profile" \
  https://example.com
```

Close the capture browser, then launch replay from the same directory, using the same profile and session:

```bash
./chrome --wootz-authoring-replay \
  --wootz-authoring-session=my-session \
  --user-data-dir="$PWD/w8-core-profile" \
  --host-resolver-rules="MAP * 0.0.0.0" \
  https://example.com
```

The replay resolver rule matches the Docker runtime's offline configuration. These native examples store the profile in a local directory; the Compose workflow stores it in a named Docker volume.

For browser observations and snapshots over CDP, see the [ChromiumRL command list](../README.md#chromiumrl-protocol). The [protocol schema](../chromium-patch/src/third_party/blink/public/devtools_protocol/domains/ChromiumRL.pdl) defines parameters and response types; the browser-side implementation is in [the inspector sources](../chromium-patch/src/third_party/blink/renderer/core/inspector/).
