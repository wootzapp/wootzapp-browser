# agent-browser integration

w8-core includes the official `agent-browser` CLI for page observations and browser actions over CDP. This directory pins the CLI dependency used in the Docker image.

## Use the bundled CLI

Use the [root README's automation commands](../README.md#browser-automation) for a page snapshot. From the directory containing `compose.yaml` and `.env`, list available actions and options:

```bash
docker compose exec w8-core agent-browser --help
```

The CLI supports actions such as navigation, clicking, filling fields, typing, pressing keys, and scrolling. An integrating application selects actions and calls the CLI; it provides any AI model, prompts, credentials, action limits, and retry logic it needs.

## Use the CLI on the host

The pinned CLI requires Node.js 24 or later. From the repository root, install the package and connect to the host CDP endpoint:

```bash
cd agent-browser
npm ci
./node_modules/.bin/agent-browser \
  --cdp http://127.0.0.1:9222 snapshot --interactive --json
```

The default host port is `9222`. If it has been changed in `.env`, use the mapped host port in the CDP URL. Observation and action commands do not require model-provider credentials.

## Version and dependencies

`package-lock.json` pins `agent-browser` to version `0.27.3`. The Docker image includes Node.js 24 and installs this pinned dependency.
