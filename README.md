# w8-core

<img src="src/chrome/app/theme/chromium/wootzapp-icon.png" alt="Wootzapp logo" width="120" />

w8-core is Wootzapp's Chromium-based desktop browser engine. This repository
contains the maintained source overlay that is applied to a clean Chromium
checkout before the final browser build.

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

## What the overlay provides

The overlay adds w8-core browser behavior while retaining Chromium's normal
desktop build and DevTools support. Its main additions include:

- ChromiumRL commands for structured browser capture and model-facing DOM;
- capture and replay support for named authoring sessions;
- offline-page support used by replay workflows;
- w8-core product names, icons, and desktop branding.

The ChromiumRL protocol schema is maintained at:

```text
src/third_party/blink/public/devtools_protocol/domains/ChromiumRL.pdl
```

Important commands include:

```text
ChromiumRL.captureStructuredSnapshot
ChromiumRL.captureSnapshotDiff
ChromiumRL.compareStructuredSnapshots
ChromiumRL.getModelDOM
```

These commands are browser-side APIs. A separate harness may call them over
the Chrome DevTools Protocol and decide how to store or use their responses.

## Authoring modes

The browser supports normal browsing, capture, and replay. Direct launches
use these flags:

```text
--wootz-authoring-capture
--wootz-authoring-replay
--wootz-authoring-session=<session-name>
```

Capture stores supported browser responses and state for the named session.
Replay serves that stored session without silently treating live network data
as recorded evidence.

## Repository layout

- `src/` contains the maintained source overlay copied onto Chromium.
- `README.md` documents the reproducible desktop build.
- `LICENSE` contains the project license.

Runtime packaging, task recording, and model credentials belong to the tool
that embeds this browser; they are intentionally not duplicated here.

## License

w8-core is licensed under the GNU Affero General Public License v3.0. See
[`LICENSE`](LICENSE).
