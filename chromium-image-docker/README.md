# w8-core Runtime

This directory builds and runs the Docker runtime for w8-core, ChromiumRL CDP,
and the pinned agent-browser command-line runtime.

The runtime starts w8-core, exposes Chrome DevTools Protocol, optionally starts a headed VNC/noVNC display stack, and provides health commands for automation.

## What the runtime does

The container entrypoint is:

```text
chromium-runtime-entrypoint
```

By default it runs:

```text
chromium-runtime-entrypoint serve
```

The serve command:

- prepares the browser profile directory
- starts Xvfb, VNC, and noVNC when headed mode is enabled
- starts w8-core
- starts a public CDP bridge
- provides Node.js and the pinned official `agent-browser` CLI
- waits until CDP is ready
- keeps the w8-core process alive

## Build payload

The w8-core payload must be present in:

```text
chromium-image-docker/build/
```

The directory must contain the Chromium executable and its runtime files, for example:

```text
build/
  chrome
  chrome_crashpad_handler
  resources.pak
  icudtl.dat
  *.so
  ...
```

The build script checks that `build/chrome` exists and is executable before building the image.
It also supplies `../agent-browser` as a named BuildKit context. That context is
restricted by `agent-browser/.dockerignore`, so local environment files and
dependencies cannot enter the image build context.

## Build the image

From this directory:

```bash
cd chromium-image-docker
./scripts/build_image.sh
```

The default image name is:

```text
w8-core:chromium-desktop
```

Override it with:

```bash
CHROMIUM_IMAGE_NAME=w8-core:chromium-desktop ./scripts/build_image.sh
```

You can also build directly with Buildx:

```bash
docker buildx build --load \
  --build-context agent_browser=../agent-browser \
  -t w8-core:chromium-desktop .
```

## Run with Docker Compose

Start w8-core:

```bash
cd chromium-image-docker
docker compose up -d
```

Check status:

```bash
docker compose exec w8-core chromium-runtime-entrypoint status
```

Wait until ready:

```bash
docker compose exec w8-core chromium-runtime-entrypoint wait --timeout 30
```

Stop the container:

```bash
docker compose down
```

## Run with helper scripts

The helper scripts use the same runtime settings as the Docker image.

Build:

```bash
./scripts/build_image.sh
```

Run:

```bash
./scripts/run_local.sh
```

Stop:

```bash
./scripts/stop_local.sh
```

## CDP ports

w8-core itself starts CDP privately on:

```text
127.0.0.1:9223
```

The runtime exposes a public CDP bridge on:

```text
0.0.0.0:9222
```

From the host, use:

```text
http://127.0.0.1:9222/json/version
```

The public CDP port is controlled by:

```env
CDP_PORT=9222
```

The private w8-core CDP port is controlled by:

```env
CHROMIUM_CDP_PORT=9223
```

Most tools should use the public port, `9222`.

## Headless and headed modes

Headless mode is the default:

```env
CHROMIUM_HEADLESS=1
```

Headed mode starts Xvfb, VNC, and noVNC:

```bash
CHROMIUM_HEADLESS=0 docker compose up -d
```

The default ports are:

```text
CDP:   9222
VNC:   5900
noVNC: 6080
```

Open noVNC in a browser:

```text
http://127.0.0.1:6080/vnc.html?host=127.0.0.1&port=6080&autoconnect=1&resize=scale
```

The display size is controlled by `VNC_SCREEN`, for example:

```env
VNC_SCREEN=1366x900x24
```

## Runtime commands

Run these inside the container:

```bash
chromium-runtime-entrypoint serve
chromium-runtime-entrypoint status
chromium-runtime-entrypoint wait --timeout 30
chromium-runtime-entrypoint version
```

`status` checks the public CDP endpoint and prints runtime information.

`wait` polls the public CDP endpoint until it is ready or the timeout expires.

`version` prints the w8-core version from the Chromium executable.

## Common configuration

The Compose file supports these runtime values:

```env
CHROMIUM_IMAGE_NAME=w8-core:chromium-desktop
CHROMIUM_CONTAINER_NAME=w8-core-browser-engine
CHROMIUM_SHM_SIZE=2g
CDP_PORT=9222
VNC_PORT=5900
NOVNC_PORT=6080
CHROMIUM_HEADLESS=1
CHROMIUM_RESET_PROFILE=1
AUTHORING_MODE=off
AUTHORING_SESSION=default
VNC_SCREEN=1366x900x24
CHROMIUM_START_URL=about:blank
CHROMIUM_EXTRA_ARGS=
```

Examples:

```bash
CHROMIUM_START_URL=https://example.com docker compose up -d
CHROMIUM_EXTRA_ARGS="--disable-features=SomeFeature" docker compose up -d
```

## Browser profile

The runtime stores browser profile data under:

```text
/home/wootz/wootz-browser-sessions
```

By default the profile is reset on each container start:

```env
CHROMIUM_RESET_PROFILE=1
```

Set it to `0` when a persistent profile is needed:

```bash
CHROMIUM_RESET_PROFILE=0 docker compose up -d
```

## Logs

Runtime metadata is written inside the container under `/tmp/chromium-runtime/`.

Important files include:

```text
/tmp/chromium-runtime/browser-launch.env
/tmp/chromium-runtime/chrome.log
```

## Authoring mode

Authoring is disabled by default:

```env
AUTHORING_MODE=off
```

Capture and replay are enabled with:

```env
AUTHORING_MODE=capture
AUTHORING_MODE=replay
AUTHORING_SESSION=<session>
```

See the browser workflow README for details:

```text
../chromium-desktop/README.md
```

## Agent-browser

The image includes the official agent-browser CLI pinned by
`../agent-browser/package-lock.json` and a compatible Node.js runtime.
It contains no model SDK, credentials, prompt, or agent loop. Each harness owns
those choices and uses agent-browser only for browser observation and actions.

Check the installed command:

```bash
docker compose exec w8-core node --version
docker compose exec w8-core agent-browser --version
```

Attach the CLI to the browser's internal CDP endpoint and take an interactive
snapshot:

```bash
docker compose exec \
  w8-core \
  agent-browser --cdp http://127.0.0.1:9222 snapshot --interactive --json
```

See `../agent-browser/README.md` for the harness integration boundary.
