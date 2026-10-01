# w8-core

<img src="chromium-desktop/src/chrome/app/theme/chromium/wootzapp-icon.png" alt="Wootzapp logo" width="120" />

w8-core is Wootzapp's desktop browser for interactive browsing, browser automation, and session capture with offline replay. The Docker image includes the browser, a web-accessible desktop through noVNC, and the `agent-browser` CLI.

## Requirements

Docker Engine must be running, with Docker Compose supporting `--wait`. The published image targets Linux `amd64`; ARM hosts require `amd64` emulation.

```bash
docker version
docker compose version
```

## Quick start

The Docker files are [compose.yaml](chromium-image-docker/compose.yaml) and [.env.example](chromium-image-docker/.env.example) in `chromium-image-docker/`. To use the published image without a source checkout, copy these two files into a working directory and run the commands there.

For an existing installation, see [existing containers](chromium-image-docker/README.md#existing-containers) before switching launch methods.

From the repository root, enter the Docker directory and create your local configuration:

```bash
cd chromium-image-docker
cp .env.example .env
```

The template contains:

```env
CHROMIUM_IMAGE_NAME=wootzapp/w8-core:latest
CHROMIUM_HEADLESS=0
CHROMIUM_RESET_PROFILE=0
CHROMIUM_START_URL=about:blank
AUTHORING_MODE=off
AUTHORING_SESSION=my-session
CDP_PORT=9222
VNC_PORT=5900
NOVNC_PORT=6080
```

Compose loads `.env` automatically. Edit this file to choose the startup page, display mode, ports, or authoring session. Exported shell variables with the same names take precedence.

Pull the image and start the browser:

```bash
docker compose pull
docker compose up -d --no-build --wait
```

Open the [browser desktop](http://127.0.0.1:6080/vnc.html?host=127.0.0.1&port=6080&autoconnect=1&resize=scale) in your web browser. The start command waits for the browser to become healthy. Run all remaining commands from the same working directory.

## Manage the browser

Check status:

```bash
docker compose ps
```

Stop the browser:

```bash
docker compose stop
```

Start it again:

```bash
docker compose start --wait
```

After changing `.env`, apply the new settings:

```bash
docker compose up -d --no-build --wait
```

Set `CHROMIUM_HEADLESS=1` for automation without a desktop. Browser data is kept in the `w8-core-profile` volume while `CHROMIUM_RESET_PROFILE=0`.

To remove the container while keeping its profile:

```bash
docker compose down
```

`docker compose down -v` also deletes the profile volume, including saved authoring sessions.

## Capture and replay a session

Capture records network responses and selected browser state. Replay serves the recorded session offline. Keep the same profile volume and session name for both modes.

### Capture

Update these settings in `.env`, replacing the session name and URL as needed:

```env
AUTHORING_MODE=capture
AUTHORING_SESSION=my-session
CHROMIUM_START_URL=https://example.com
```

Start capture, then open the browser desktop and browse the pages and interactions you want to record:

```bash
docker compose up -d --no-build --wait
```

### Replay

Change `AUTHORING_MODE` in `.env` to `replay`, keeping the session name and startup URL unchanged:

```env
AUTHORING_MODE=replay
```

Apply the settings and revisit the recorded pages:

```bash
docker compose up -d --no-build --wait
```

To resume normal browsing, set `AUTHORING_MODE=off` and run the same command. See the [authoring guide](chromium-desktop/README.md) for recording verification and capture/replay behavior.

### Command-line alternative

You can select a mode for a launch without editing `.env`.

Capture:

```bash
AUTHORING_MODE=capture \
docker compose up -d --no-build --wait
```

Replay, after completing the capture:

```bash
AUTHORING_MODE=replay \
docker compose up -d --no-build --wait
```

Both commands reuse the session, startup URL, and profile settings from `.env`. The override applies only to that command; later launches use the mode saved in `.env`.

For direct browser launches, the flags are `--wootz-authoring-capture` or `--wootz-authoring-replay`, together with `--wootz-authoring-session=<session>`. See the [native browser examples](chromium-desktop/README.md#native-browser-flags).

## Browser automation

The default connections are:

| Interface | Address |
|---|---|
| DevTools Protocol (CDP) | `http://127.0.0.1:9222/json/version` |
| Browser desktop (noVNC) | `http://127.0.0.1:6080/vnc.html` |
| VNC client | `127.0.0.1:5900` |

Use `webSocketDebuggerUrl` from the CDP endpoint with your CDP client. If you change a port in `.env`, use that port in the corresponding address. Ports are bound to the Docker host's loopback address; forward them through SSH or your editor when Docker runs on a remote host.

Check the bundled `agent-browser` version and capture an interactive page snapshot:

```bash
docker compose exec w8-core agent-browser --version
docker compose exec w8-core agent-browser --cdp 9222 \
  snapshot --interactive --json
```

See the [integration guide](agent-browser/README.md) for additional usage and host integration. An integrating application supplies its own AI model and chooses the actions.

## ChromiumRL protocol

The ChromiumRL CDP domain provides structured observations, snapshots, and comparisons. Its commands are:

```text
ChromiumRL.enable
ChromiumRL.disable
ChromiumRL.getTouchTraces
ChromiumRL.getLayoutTimings
ChromiumRL.getCLSAttribution
ChromiumRL.getCompositorLayers
ChromiumRL.captureInteraction
ChromiumRL.getVisualHash
ChromiumRL.captureStateSnapshot
ChromiumRL.computeStateDiff
ChromiumRL.saveDOMState
ChromiumRL.compareDOMState
ChromiumRL.getAgentObservation
ChromiumRL.captureStructuredSnapshot
ChromiumRL.compareStructuredSnapshots
ChromiumRL.captureSnapshotDiff
ChromiumRL.getModelDOM
ChromiumRL.startDOMDiff
ChromiumRL.stopDOMDiff
```

Parameters and response types are defined in the [protocol schema](chromium-patch/src/third_party/blink/public/devtools_protocol/domains/ChromiumRL.pdl).

## Further documentation

- [Environment settings, logs, and local image builds](chromium-image-docker/README.md)
- [Recording verification and replay behavior](chromium-desktop/README.md)
- [agent-browser observations and actions](agent-browser/README.md)
