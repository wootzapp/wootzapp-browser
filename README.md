# Wootzapp Browser

<img src="src/chrome/app/theme/chromium/wootzapp-icon.png" alt="Wootzapp logo" width="120" />

**w8-core** is the Chromium-based engine behind Wootzapp Browser. It supports
interactive browsing, automation, capture, and replay. This repository
contains the maintained source overlay that is applied to a clean Chromium
checkout before the final browser build.

## What Wootzapp Browser provides

- A normal headed desktop browser built on Chromium.
- A headless mode for automation and CI.
- Chrome DevTools Protocol access for external browser tools.
- The ChromiumRL CDP domain for structured capture, observations, comparison,
  layout signals, and model-facing DOM data.
- Named authoring sessions that capture supported network and browser state.
- Offline replay from a previously captured authoring session.

## System requirements

- A 64-bit Linux system, or WSL 2 on Windows.
- At least 32 GiB RAM and 200 GiB free disk space.
- Git, Python 3, and the packages installed by Chromium's dependency script.
- A fast Linux filesystem. On WSL, avoid building under `/mnt/c`.

Do not apply the overlay to a Chromium checkout containing unrelated local
changes. Keep the Chromium checkout and this repository beside each other.

## Build Chromium

### 1. Install depot_tools

Create a workspace and clone Chromium's build tools:

```bash
mkdir -p w8-workspace
cd w8-workspace
git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git
export PATH="$PATH:$(pwd)/depot_tools"
```

Add the `export` command to your shell profile if you want `depot_tools`
available in new terminals.

### 2. Fetch the official source

```bash
mkdir -p chromium
cd chromium
fetch --nohooks --no-history chromium
gclient sync --nohooks \
  --revision src@refs/tags/152.0.7948.0 \
  --reset --upstream -D --force --no-history --shallow
```

The fetch creates `chromium/src/` and the `.gclient` configuration.

### 3. Install dependencies and run hooks

```bash
cd src
./build/install-build-deps.sh
gclient runhooks
```

### 4. Build official Chromium first

Generate the Release build with visible, reproducible arguments:

```bash
gn gen out/Release --args='is_debug=false
is_component_build=false
symbol_level=0
blink_symbol_level=0
v8_symbol_level=0
proprietary_codecs=true
ffmpeg_branding="Chrome"'
autoninja -C out/Release chrome
./out/Release/chrome --version
```

Do not apply the w8-core files until this build succeeds. This separates
upstream setup failures from w8-core source failures.

## Build w8-core

### 5. Clone the w8-core source overlay

Return to the workspace and clone this repository:

```bash
cd ../..
git clone --depth 1 https://github.com/wootzapp/wootzapp-browser.git
```

The workspace now contains `chromium/`, `depot_tools/`, and
`wootzapp-browser/` beside each other.

### 6. Apply the overlay

```bash
sudo chmod -R u+w chromium/src/
cp -r wootzapp-browser/src/* chromium/src/
```

Only files present under `wootzapp-browser/src/` are copied. The repository
does not contain or replace the complete Chromium checkout.

### 7. Rebuild the browser

```bash
cd chromium
gclient runhooks
cd src
gn gen out/Release
autoninja -C out/Release chrome
./out/Release/chrome --version
```

The second build reuses the Release arguments stored in `out/Release`.

## Run the built browser

Run these commands from the Chromium `src/` directory after the w8-core
rebuild completes.

### Headed mode

Headed mode opens the normal desktop window and exposes CDP locally:

```bash
./out/Release/chrome --user-data-dir=/tmp/w8-core-profile --remote-debugging-address=127.0.0.1 --remote-debugging-port=9222 '--remote-allow-origins=*' about:blank
```

This requires a working desktop display. Use a different `--user-data-dir`
when another browser process is already using this profile. For a remote
server, use the Docker/noVNC browser harness documented in the
[`w8-domdiff-bench` runbook](https://github.com/wootzapp/w8-domdiff-bench/blob/w8-reproducible/docs/runbook.md#browser-runtime).

### Headless mode

Headless mode runs without a visible browser window:

```bash
./out/Release/chrome --headless=new --disable-gpu --user-data-dir=/tmp/w8-core-profile --remote-debugging-address=127.0.0.1 --remote-debugging-port=9222 '--remote-allow-origins=*' about:blank
```

Replace `about:blank` with a URL to open a specific page. Add other Chromium
flags before the URL.

### Check CDP

While the browser is running, open:

```text
http://127.0.0.1:9222/json/version
```

The response includes `webSocketDebuggerUrl`, which CDP clients use to
connect to the browser.

### Docker and noVNC

The benchmark repository packages a local w8-core build into an image and
assigns conflict-free host ports. Follow its browser-build instructions
first. Once its Python package and runtime configuration exist, run these
commands from the `w8-domdiff-bench` repository root:

```bash
source .venv/bin/activate
python -m w8bench.task_recorder.browser_runtime configure
docker compose --env-file .env up -d --wait w8-core
python -m w8bench.task_recorder.browser_runtime status
```

The status command prints the CDP and noVNC URLs. It also verifies the
browser, container health, and bundled agent-browser command.

Stop this checkout's container with:

```bash
docker compose --env-file .env down
```

## What the overlay provides

The overlay adds w8-core browser behavior while retaining Chromium's normal
desktop build and DevTools support. Its main additions include:

- ChromiumRL commands for structured browser capture and model-facing DOM;
- capture and replay support for named authoring sessions;
- offline-page support used by replay workflows;
- w8-core product names, icons, and desktop branding.

### ChromiumRL CDP domain

The ChromiumRL protocol schema is maintained at:

```text
src/third_party/blink/public/devtools_protocol/domains/ChromiumRL.pdl
```

The complete command surface is:

| Command | Purpose |
| --- | --- |
| `ChromiumRL.enable` | Starts ChromiumRL tracing and selects the touch, layout, CLS, and compositor signals to collect. |
| `ChromiumRL.disable` | Stops ChromiumRL tracing. |
| `ChromiumRL.getTouchTraces` | Returns captured touch events and the elements attributed to them. |
| `ChromiumRL.getLayoutTimings` | Returns per-element and frame-level layout timing information. |
| `ChromiumRL.getCLSAttribution` | Returns cumulative layout-shift entries and their attributed elements. |
| `ChromiumRL.getCompositorLayers` | Returns compositor-layer information, including optional paint and transform details. |
| `ChromiumRL.captureInteraction` | Collects touch, layout, CLS, and compositor signals around one interaction. |
| `ChromiumRL.getVisualHash` | Returns a hash of the current visual state and whether a visual update occurred. |
| `ChromiumRL.captureStateSnapshot` | Captures the semantic and visual node state used by the earlier state-comparison API. |
| `ChromiumRL.computeStateDiff` | Compares the current page with a supplied state snapshot. |
| `ChromiumRL.saveDOMState` | Captures a rich DOM state containing styles, XPath information, and layout geometry. |
| `ChromiumRL.compareDOMState` | Compares a saved rich DOM state with the current live page. |
| `ChromiumRL.getAgentObservation` | Returns interactive elements and optional page content for browser navigation. |
| `ChromiumRL.captureStructuredSnapshot` | Captures the structured page graph from DOM, accessibility, and layout data. |
| `ChromiumRL.compareStructuredSnapshots` | Compares two supplied structured snapshots without reading the live document. |
| `ChromiumRL.captureSnapshotDiff` | Captures the live after-state and compares it with a supplied before-snapshot in one call. |
| `ChromiumRL.getModelDOM` | Builds model-facing structured DOM data from a supplied snapshot without recapturing the page. |
| `ChromiumRL.startDOMDiff` | Starts live DOM-difference tracking. Its first report represents the initial state. |
| `ChromiumRL.stopDOMDiff` | Stops live DOM-difference tracking. |

The domain can also emit `domDiffOccurred`, `layoutShiftDetected`,
`touchInteractionCompleted`, and `layerTreeChanged` events. The exact command
parameters, return objects, events, and type definitions are in
`ChromiumRL.pdl`. A tool does not need to use every command; it selects the
part of the protocol required by its workflow.

These commands are browser-side APIs. A separate harness may call them over
the Chrome DevTools Protocol and decide how to store or use their responses.

### Structured capture example

After connecting to the WebSocket URL returned by `/json/version`, a CDP
client can send:

```json
{
  "id": 1,
  "method": "ChromiumRL.captureStructuredSnapshot",
  "params": {
    "inViewportOnly": true,
    "includeOffscreen": false
  }
}
```

The returned `result.snapshot` is structured browser data. To build the
model-facing representation from that exact snapshot, send it back without
recapturing the live page:

```json
{
  "id": 2,
  "method": "ChromiumRL.getModelDOM",
  "params": {
    "snapshot": {
      "...": "complete result.snapshot object"
    }
  }
}
```

The placeholder represents the complete snapshot object, not a string.

## Authoring modes

The browser supports normal browsing, capture, and replay.

### Normal browsing

Launch without either authoring mode flag to browse normally. No authoring
archive is written or read:

```bash
./out/Release/chrome --user-data-dir=/tmp/w8-core-normal https://example.com
```

### Session configuration

Direct capture and replay launches use these flags:

```text
--wootz-authoring-capture
--wootz-authoring-replay
--wootz-authoring-session=<session-name>
```

Capture stores supported browser responses and state for the named session.
Replay serves that stored session without silently treating live network data
as recorded evidence. If no session is supplied, the browser uses `default`.
Capture and replay must use the same session name and the same browser
profile. Using a different profile points the browser at a different archive.

### Start capture mode

Run capture with a dedicated profile and a named session:

```bash
./out/Release/chrome --user-data-dir=/tmp/w8-core-authoring --remote-debugging-port=9222 --wootz-authoring-capture --wootz-authoring-session=my-session https://example.com
```

Captured data is stored under:

```text
/tmp/w8-core-authoring/Default/WootzAuthoring/sessions/my-session/
```

The archive contains an `index.jsonl` request index and stored payloads.
Verify that capture produced data before starting replay:

```bash
test -s /tmp/w8-core-authoring/Default/WootzAuthoring/sessions/my-session/index.jsonl
find /tmp/w8-core-authoring/Default/WootzAuthoring/sessions/my-session -maxdepth 2 -type f
```

### What capture stores

Capture records supported HTTP and HTTPS responses, redirects, response
payloads, same-document routes used by single-page applications,
`localStorage` for visited origins, and cookies for visited hosts. Main
document loading bypasses service workers and the HTTP cache so supported
requests reach the capture path.

At capture startup, w8-core clears IndexedDB, CacheStorage, and service
workers in the default storage partition. It keeps existing cookies and
`localStorage` so the selected browser profile can retain sign-in and site
state.

### Start replay mode

Close the capture browser, then reuse the same profile and session:

```bash
./out/Release/chrome --user-data-dir=/tmp/w8-core-authoring --remote-debugging-port=9222 --wootz-authoring-replay --wootz-authoring-session=my-session '--host-resolver-rules=MAP * 0.0.0.0' https://example.com
```

The resolver rule prevents an unintended live-network fallback during an
offline replay. A different profile or session name will not contain the
captured data.

### What replay restores

Replay restores archived cookies and `localStorage`, serves stored responses
and redirects, supports captured same-document routes, and provides a
synthetic WebSocket connection for offline use. Before restoring the archive,
it clears the corresponding live application state so that stale state does
not override the recording.

If a request cannot be matched in strict replay, w8-core returns a replay-miss
response instead of silently fetching that resource from the network. The
host-resolver rule in the example provides an additional network boundary for
the direct browser launch.

Closing and reopening the browser does not remove the authoring session as
long as the same `--user-data-dir` remains on disk. Deleting that profile also
deletes the stored sessions inside it.

## Repository layout

- `src/` contains the maintained source overlay copied onto Chromium.
- `README.md` documents the reproducible desktop build.
- `LICENSE` contains the project license.

Runtime packaging, task recording, and model credentials belong to the tool
that embeds this browser; they are intentionally not duplicated here.

## License

The w8-core source overlay is licensed under the GNU Affero General Public
License v3.0. See [`LICENSE`](LICENSE).
