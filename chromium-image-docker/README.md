# w8-core Docker reference

Use the [root README](../README.md#quick-start) for the `.env` setup and basic browsing, capture, and replay commands. This reference covers additional settings, diagnostics, and local image builds.

Unless stated otherwise, run commands from `chromium-image-docker/`, where `compose.yaml` and your `.env` file are stored.

## Environment configuration

Compose reads `.env` automatically. Shell variables override values in the file. After editing `.env`, apply the settings with `docker compose up -d --no-build --wait`; `docker compose restart` does not apply environment changes.

Check that the configuration is valid:

```bash
docker compose config --quiet
```

The Compose defaults below apply when a setting is absent. `.env.example` selects the published image, desktop mode, profile retention, and `AUTHORING_SESSION=my-session`.

| Setting | Default | Purpose |
|---|---|---|
| `CHROMIUM_IMAGE_NAME` | `w8-core:chromium-desktop` | Published image or a local image tag. |
| `CHROMIUM_CONTAINER_NAME` | `w8-core-browser-engine` | Container name. |
| `CHROMIUM_SHM_SIZE` | `2g` | Shared memory available to the browser. |
| `CHROMIUM_PROFILE_VOLUME` | `w8-core-profile` | Named volume containing the browser profile. |
| `CDP_PORT` | `9222` | Host port for CDP. |
| `VNC_PORT` | `5900` | Host port for VNC clients. |
| `NOVNC_PORT` | `6080` | Host port for the web desktop. |
| `CHROMIUM_HEADLESS` | `1` | Set to `0` to enable the desktop. |
| `CHROMIUM_RESET_PROFILE` | `1` | Set to `0` to retain the profile at startup. |
| `AUTHORING_MODE` | `off` | `off`, `capture`, or `replay`. |
| `AUTHORING_SESSION` | `default` | Name of the capture/replay session. |
| `VNC_SCREEN` | `1366x900x24` | Desktop width, height, and color depth. |
| `CHROMIUM_START_URL` | `about:blank` | Page to open at startup. |
| `CHROMIUM_EXTRA_ARGS` | Empty | Additional browser flags. |

For example, add a browser flag in `.env`:

```env
CHROMIUM_EXTRA_ARGS=--disable-features=SomeFeature
```

The profile is mounted at `/home/wootz/wootz-browser-sessions`. Keep profile resets disabled to retain browsing data and recordings. `docker compose down` keeps the volume; `docker compose down -v` removes it.

## Connections

Compose binds the host ports to `127.0.0.1`. Use the port values from `.env` when connecting. For remote Docker hosts, forward the required ports through SSH or your editor.

Inside the container, the browser listens on private CDP port `9223`, behind the public bridge on `9222`. Clients should use the public endpoint. Direct Docker launches can override the private port with `--env CHROMIUM_CDP_PORT=<port>`; the Compose file does not forward that setting.

## Status and logs

The image includes a health check used by Compose's `--wait` option. The entrypoint, `chromium-runtime-entrypoint`, also provides these diagnostic commands:

```bash
docker compose exec w8-core chromium-runtime-entrypoint status
docker compose exec w8-core chromium-runtime-entrypoint wait --timeout 30
docker compose exec w8-core chromium-runtime-entrypoint version
```

Launch settings and browser logs are stored inside the container:

```text
/tmp/chromium-runtime/browser-launch.env
/tmp/chromium-runtime/chrome.log
```

Read the browser log:

```bash
docker compose exec w8-core cat /tmp/chromium-runtime/chrome.log
```

Check the bundled Node.js version:

```bash
docker compose exec w8-core node --version
```

See the [agent-browser guide](../agent-browser/README.md) for observation and action commands.

## Existing containers

If `w8-core-browser-engine` already exists from a direct `docker run` launch, stop and remove it before starting the Compose workflow:

```bash
docker stop w8-core-browser-engine
docker rm w8-core-browser-engine
```

Its `w8-core-profile` named volume can be reused by this configuration. If another Compose project manages the container, stop that project through its own configuration first. To run a separate browser instance, choose a different container name, profile volume, and host ports in `.env`.

## Local image build

A local image build packages an already compiled browser. From the repository root, place the executable and all required runtime files under `chromium-image-docker/build/`:

```text
build/
  chrome
  chrome_crashpad_handler
  resources.pak
  icudtl.dat
  *.so
  ...
```

The executable `build/chrome` must have execute permission. The build script checks the executable and includes the pinned `../agent-browser` package through a named BuildKit context. `agent-browser/.dockerignore` excludes local environment files and dependencies.

Build the local image:

```bash
cd chromium-image-docker
CHROMIUM_IMAGE_NAME=w8-core:chromium-desktop \
  ./scripts/build_image.sh
```

Set `CHROMIUM_IMAGE_NAME=w8-core:chromium-desktop` in this directory's `.env`, then start the image:

```bash
docker compose up -d --no-build --wait
```

For a custom image tag, use the same tag when building and in `.env`. Browser source changes require rebuilding the browser payload before packaging the image.

To build directly with Buildx, run from `chromium-image-docker/`:

```bash
docker buildx build --load \
  --build-context agent_browser=../agent-browser \
  -t "${CHROMIUM_IMAGE_NAME:-w8-core:chromium-desktop}" .
```

### Helper scripts

Run the direct Docker helpers from `chromium-image-docker/`:

| Action | Command |
|---|---|
| Start | `./scripts/run_local.sh` |
| Stop and remove | `./scripts/stop_local.sh` |

The start helper does not mount a profile volume. Use the Compose workflow when data must survive container replacement.
