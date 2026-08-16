# Wootzapp Desktop Browser

Wootzapp Desktop Browser is a Chromium-based desktop browser runtime for normal browsing, browser automation, browser-side capture, and repeatable authoring workflows.

The browser can be started as a Dockerized desktop browser, used visually through noVNC, controlled through Chrome DevTools Protocol, and extended through ChromiumRL CDP commands for structured browser state capture.

## What you can do

- Run the browser in headless mode for automation.
- Run the browser in headed mode and open it through noVNC.
- Connect automation tools through CDP on port `9222`.
- Use ChromiumRL CDP commands for browser observation and capture.
- Use authoring capture mode to archive a web session.
- Use authoring replay mode to replay a captured session offline.
- Use optional agent-browser automation when the image includes it.

## Quick start

From the repository root:

```bash
cd chromium-image-docker
docker compose up -d
```

Check that the browser is ready:

```bash
docker compose exec wootzapp-chromium-desktop chromium-runtime-entrypoint status
```

Open the CDP version endpoint from the host:

```text
http://127.0.0.1:9222/json/version
```

Stop the browser:

```bash
docker compose down
```

## Build the Docker image

The Docker image requires a prepared browser payload under:

```text
chromium-image-docker/build/
```

At minimum, the payload must contain the browser executable:

```text
chromium-image-docker/build/chrome
```

Build the image:

```bash
cd chromium-image-docker
./scripts/build_image.sh
```

Use a custom image name:

```bash
CHROMIUM_IMAGE_NAME=wootzapp-chromium-desktop:local ./scripts/build_image.sh
```

## Run in headless mode

Headless mode is the default and is useful for automation or CI.

```bash
cd chromium-image-docker
CHROMIUM_HEADLESS=1 docker compose up -d
```

The browser exposes CDP on the host at:

```text
127.0.0.1:9222
```

## Run in headed mode

Headed mode starts the browser with an Xvfb display, VNC, and noVNC.

```bash
cd chromium-image-docker
CHROMIUM_HEADLESS=0 docker compose up -d
```

Open noVNC:

```text
http://127.0.0.1:6080/vnc.html?host=127.0.0.1&port=6080&autoconnect=1&resize=scale
```

Default runtime ports:

```text
CDP:   9222
VNC:   5900
noVNC: 6080
```

## Start on a specific page

Use `CHROMIUM_START_URL` to choose the initial page:

```bash
cd chromium-image-docker
CHROMIUM_START_URL=https://example.com docker compose up -d
```

Use `CHROMIUM_EXTRA_ARGS` for additional Chromium flags:

```bash
CHROMIUM_EXTRA_ARGS="--disable-features=SomeFeature" docker compose up -d
```

## Runtime commands

The container entrypoint is `chromium-runtime-entrypoint`.

Useful commands inside the running container:

```bash
chromium-runtime-entrypoint status
chromium-runtime-entrypoint wait --timeout 30
chromium-runtime-entrypoint version
```

With Docker Compose:

```bash
docker compose exec wootzapp-chromium-desktop chromium-runtime-entrypoint status
docker compose exec wootzapp-chromium-desktop chromium-runtime-entrypoint wait --timeout 30
docker compose exec wootzapp-chromium-desktop chromium-runtime-entrypoint version
```

## CDP automation

The browser starts Chromium's private debugging endpoint inside the container and exposes a public CDP bridge on port `9222`.

Use this host endpoint for automation tools:

```text
http://127.0.0.1:9222/json/version
```

Most CDP clients can connect by reading the `webSocketDebuggerUrl` from that endpoint.

## ChromiumRL CDP

ChromiumRL CDP is a custom DevTools Protocol domain exposed by Wootzapp Desktop Browser.

It provides browser-side commands for structured capture and observation, including:

```text
ChromiumRL.enable
ChromiumRL.getTouchTraces
ChromiumRL.getLayoutTimings
ChromiumRL.getCLSAttribution
ChromiumRL.getCompositorLayers
ChromiumRL.captureInteraction
ChromiumRL.captureStateSnapshot
ChromiumRL.computeStateDiff
ChromiumRL.saveDOMState
ChromiumRL.compareDOMState
ChromiumRL.getAgentObservation
ChromiumRL.captureStructuredSnapshot
ChromiumRL.startDOMDiff
ChromiumRL.stopDOMDiff
```

Typical ChromiumRL usage:

```text
1. Start the browser runtime.
2. Connect to CDP on 127.0.0.1:9222.
3. Send ChromiumRL commands through a CDP client.
4. Store or process the returned browser state in your tool.
```

## Authoring capture

Authoring capture records browser-side network responses and related browser state for a named session.

Start capture mode:

```bash
cd chromium-image-docker

AUTHORING_MODE=capture AUTHORING_SESSION=my-session CHROMIUM_START_URL=https://example.com CHROMIUM_HEADLESS=0 docker compose up -d
```

Capture mode records HTTP/HTTPS responses, redirects, response payloads, selected browser state, and same-document SPA routes.

## Authoring replay

Authoring replay serves a previously captured session back through the browser.

Start replay mode:

```bash
cd chromium-image-docker

AUTHORING_MODE=replay AUTHORING_SESSION=my-session CHROMIUM_START_URL=https://example.com CHROMIUM_HEADLESS=0 docker compose up -d
```

Replay mode loads the captured session, restores captured state, serves matching captured responses, and avoids silently falling back to the live network on strict replay misses.

## Optional agent-browser automation

Some browser images include optional `agent-browser` support for model-driven browser automation.

When available, provide model credentials at runtime and run a natural-language task inside the existing browser container:

```bash
cd chromium-image-docker

docker compose --env-file .env.agent-browser up -d

docker compose --env-file .env.agent-browser exec wootzapp-chromium-desktop   agent-browser task "go to https://example.com and summarize the page"
```

agent-browser is not started automatically. Normal browsing and authoring workflows are unchanged unless this command is explicitly run.

## Common environment variables

```env
CHROMIUM_IMAGE_NAME=devjangid/wootzapp-chromium-desktop:local
CHROMIUM_CONTAINER_NAME=wootzapp-chromium-desktop
CHROMIUM_HEADLESS=1
CHROMIUM_RESET_PROFILE=1
CHROMIUM_START_URL=about:blank
CHROMIUM_EXTRA_ARGS=
CDP_PORT=9222
VNC_PORT=5900
NOVNC_PORT=6080
AUTHORING_MODE=off
AUTHORING_SESSION=default
```

## More documentation

Runtime details:

```text
chromium-image-docker/README.md
```

Browsing, capturing, and authoring details:

```text
chromium-desktop/README.md
```

Optional agent-browser details, when included:

```text
agent-browser/README.md
```
