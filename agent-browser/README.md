# Standalone agent-browser runner

This directory provides an optional agent-browser runner for Wootzapp Desktop Browser.

It does not start the browser and it is not part of the normal browser Docker runtime. The browser must already be running. agent-browser connects to that running browser through the browser's CDP endpoint.

## How it connects

The normal browser Docker flow starts the browser container and exposes CDP on the host.

Default host CDP endpoint:

```text
http://127.0.0.1:9222
```

agent-browser attaches to that endpoint and sends browser actions through CDP.

```text
running browser container -> CDP 9222 -> standalone agent-browser
```

## Install dependencies

From the repository root:

```bash
cd agent-browser
npm install
```

## Configure credentials

Copy the example environment file:

```bash
cp .env.example .env.agent-browser
```

Set at least:

```env
OPENAI_API_KEY=your_key_here
OPENAI_MODEL=gpt-4o
AGENT_BROWSER_CDP_TARGET=http://127.0.0.1:9222
```

Do not commit real API keys.

## Start the browser

Start Wootzapp Desktop Browser through the normal Docker runtime:

```bash
cd chromium-image-docker
docker compose up -d
```

Check that CDP is reachable:

```bash
curl http://127.0.0.1:9222/json/version
```

## Run a task

From the repository root:

```bash
cd agent-browser
set -a
. ./.env.agent-browser
set +a
node ./bin/agent-browser-desktop.mjs task "go to https://example.com and summarize the page"
```

Or run directly with environment variables:

```bash
cd agent-browser
OPENAI_API_KEY="$OPENAI_API_KEY" OPENAI_MODEL=gpt-4o AGENT_BROWSER_CDP_TARGET=http://127.0.0.1:9222 node ./bin/agent-browser-desktop.mjs task "go to https://example.com and summarize the page"
```

## What happens

The task runner:

1. connects to the already-running browser through CDP
2. takes a browser snapshot
3. sends the snapshot and task instruction to the configured model
4. executes one browser action through agent-browser core
5. repeats until the task is complete or the step limit is reached

Normal browser usage is unchanged. agent-browser only runs when explicitly invoked.
