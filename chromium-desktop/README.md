# Browsing, Capturing, and Authoring

This document explains the main browser workflows supported by w8-core.

The same desktop browser runtime can be used for normal browsing, CDP-based capture, and authoring capture/replay. The mode is selected through browser runtime configuration and command-line switches.

## Browsing

Browsing is the default mode.

In this mode, w8-core starts normally and loads the configured start URL. Authoring is disabled unless explicitly enabled.

The Docker runtime uses:

```env
AUTHORING_MODE=off
CHROMIUM_START_URL=about:blank
CHROMIUM_HEADLESS=1
```

To start a normal browser session with Docker Compose:

```bash
cd chromium-image-docker
docker compose up -d
docker compose exec w8-core chromium-runtime-entrypoint status
```

To start on a specific page:

```bash
CHROMIUM_START_URL=https://example.com docker compose up -d
```

To use w8-core visually, start headed mode:

```bash
CHROMIUM_HEADLESS=0 docker compose up -d
```

Then open noVNC:

```text
http://127.0.0.1:6080/vnc.html?host=127.0.0.1&port=6080&autoconnect=1&resize=scale
```

## CDP access

The runtime exposes Chrome DevTools Protocol on:

```text
http://127.0.0.1:9222/json/version
```

Inside the container, Chromium runs its private debugging endpoint on port `9223`. The runtime exposes a public CDP bridge on port `9222`.

Automation tools should connect to the public CDP endpoint:

```text
127.0.0.1:9222
```

The runtime status command also reports the CDP WebSocket debugger URL:

```bash
docker compose exec w8-core chromium-runtime-entrypoint status
```

## Capturing with ChromiumRL CDP

ChromiumRL CDP is w8-core's custom DevTools Protocol domain for structured browser observation and capture.

It is useful when a tool needs browser-side information instead of only raw page HTML or screenshots.

The protocol definition is maintained under:

```text
chromium-patch/src/third_party/blink/public/devtools_protocol/domains/ChromiumRL.pdl
```

The browser-side implementation is under:

```text
chromium-patch/src/third_party/blink/renderer/core/inspector/
```

Important ChromiumRL commands include:

```text
ChromiumRL.enable
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
ChromiumRL.getModelDOM
ChromiumRL.startDOMDiff
ChromiumRL.stopDOMDiff
```

`ChromiumRL.getModelDOM` builds structured model-facing DOM data from a
snapshot previously captured with `ChromiumRL.captureStructuredSnapshot`. It
takes the captured `StructuredPageSnapshot` as input and returns a `ModelDOM`
value containing a renderer name and a list of named sections. The command
works only on the captured snapshot; it does not read or mutate the live
document.

Typical capture flow:

```text
1. Start the w8-core runtime.
2. Connect to CDP on port 9222.
3. Enable or call the required ChromiumRL command.
4. Store the returned browser state in the calling tool.
```

The exact CDP client can vary. Any client that can send raw DevTools Protocol commands can call the ChromiumRL domain.

## Authoring overview

Authoring is a browser-side capture and replay system.

It is controlled by two runtime values:

```env
AUTHORING_MODE=off
AUTHORING_SESSION=default
```

Valid modes are:

```text
off
capture
replay
```

The runtime translates these modes into browser switches:

```text
--wootz-authoring-capture
--wootz-authoring-replay
--wootz-authoring-session=<session>
```

Authoring is disabled by default. Normal browsing does not capture or replay traffic.

## Authoring capture mode

Capture mode records network responses and browser state for a named session.

Start capture mode with:

```bash
cd chromium-image-docker

AUTHORING_MODE=capture \
AUTHORING_SESSION=my-session \
CHROMIUM_START_URL=https://example.com \
CHROMIUM_HEADLESS=0 \
docker compose up -d
```

In capture mode, w8-core:

- records HTTP and HTTPS responses
- records redirects
- stores response payloads
- writes session metadata to an index
- captures localStorage for visited origins
- captures cookies for visited hosts
- records same-document SPA routes
- bypasses service worker and HTTP cache for main document loading

At capture startup, w8-core clears IndexedDB, CacheStorage, and service workers for the profile's default storage partition. Cookies and localStorage are not cleared by that cold-capture cleanup.

Captured data is stored under the browser profile's authoring storage area. In the desktop runtime this is based on the profile directory used by the w8-core container.

## Authoring replay mode

Replay mode serves previously captured responses from the named authoring session.

Start replay mode with:

```bash
cd chromium-image-docker

AUTHORING_MODE=replay \
AUTHORING_SESSION=my-session \
CHROMIUM_START_URL=https://example.com \
CHROMIUM_HEADLESS=0 \
docker compose up -d
```

In replay mode, w8-core:

- loads captured response records
- restores captured localStorage and cookies
- serves matching captured responses instead of live network responses
- supports redirect replay
- uses fallback matching for some document and static asset requests
- handles same-document SPA route replay
- provides a synthetic WebSocket connection for offline replay

If a request cannot be matched during strict replay, w8-core returns a replay miss response instead of silently going to the live network.

## Authoring sessions

Use stable session names when capturing and replaying:

```env
AUTHORING_SESSION=my-session
```

Session names should use simple characters:

```text
letters, numbers, dot, underscore, dash
```

The runtime rejects empty session names and names with unsupported characters.

## Choosing the right mode

Use normal browsing when you only need a browser:

```env
AUTHORING_MODE=off
```

Use ChromiumRL CDP capture when an external tool needs structured browser state:

```text
connect to CDP -> call ChromiumRL commands
```

Use authoring capture/replay when you need a browser-side offline archive of a web session:

```env
AUTHORING_MODE=capture
AUTHORING_MODE=replay
```

These workflows can share the same Docker runtime, but they should be enabled intentionally for each run.
