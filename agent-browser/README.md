# Agent-browser action runtime

This directory pins the official agent-browser CLI included in the Wootzapp
Desktop Browser image. The CLI connects to w8-core over CDP and provides
browser observations and actions.

## Responsibility boundary

The image provides:

- w8-core and ChromiumRL
- the CDP service
- Node.js
- the pinned official agent-browser action CLI

The calling harness provides:

- the AI model and provider client
- prompts and response parsing
- action limits, timeouts, retries, and stopping rules
- recording and evaluation logic

No model SDK, model name, API credential, prompt, or agent loop is embedded in
this directory or the browser image. A harness can therefore use GPT, Gemini,
Claude, Fara, a local model, or any other policy that can choose actions.

## Version

package-lock.json pins agent-browser to version 0.27.3. Node.js 24 is provided
by the browser image because that version of agent-browser requires Node.js 24
or newer.

## Use from the browser image

Build and start the image as documented in chromium-image-docker/README.md,
then verify the CLI:

    docker compose -f ../chromium-image-docker/compose.yaml exec w8-core \
      agent-browser --version

The browser listens on port 9222 inside the container. A harness can invoke the
bundled CLI directly, for example:

    docker compose -f ../chromium-image-docker/compose.yaml exec w8-core \
      agent-browser --cdp http://127.0.0.1:9222 snapshot --interactive --json

The harness should call the corresponding agent-browser commands for actions
such as click, fill, type, press, scroll, and navigation. It should pass the
selected action; agent-browser does not select actions or call a model.

## Use from a host harness

A harness may instead install the pinned package from this directory and connect
to the host-mapped CDP port:

    cd agent-browser
    npm ci
    ./node_modules/.bin/agent-browser \
      --cdp http://127.0.0.1:9222 snapshot --interactive --json

The host port is controlled by the Docker runtime's CDP_PORT setting. No
provider credentials are needed by agent-browser for observation or action
commands.
