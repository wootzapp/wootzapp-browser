# Optional agent-browser runtime

This directory adds an optional `agent-browser` task runner to the Wootz Chromium desktop browser image.

The desktop browser starts normally. `agent-browser` is available inside the running browser container for users or tools that want model-driven browser task execution. It is not started automatically and does not change the normal browser runtime.

## Configuration

Copy the example environment file and fill in the credentials at runtime:

```bash
cp agent-browser/.env.example .env.agent-browser
```

At least one model provider key is required for task mode. OpenAI is supported through:

```env
OPENAI_API_KEY=
OPENAI_MODEL=gpt-4o
```

The task runner connects to the desktop browser through the container-local CDP target:

```env
AGENT_BROWSER_CDP_TARGET=9222
```

Do not commit real API keys.

## Usage

Start the desktop browser image with the environment file:

```bash
docker compose --env-file .env.agent-browser up -d
```

Run a natural-language browser task inside the same container:

```bash
docker compose --env-file .env.agent-browser exec wootzapp-chromium-desktop \
  agent-browser task "go to https://example.com and summarize the page"
```

The task runner observes the page, asks the configured model for the next browser action, executes the action against the existing Wootz Chromium desktop browser, and repeats until the task is complete or `AGENT_BROWSER_MAX_STEPS` is reached.

## Notes

The upstream source repository is not vendored into this image. Package dependencies are pinned through `package.json` and `package-lock.json` for reproducible image builds.

Do not bake API keys into the image. Provide them at runtime through an env file, Compose environment, or Docker exec environment.

Do not use browser shutdown as routine cleanup for task execution. Stop the Docker container through the normal Docker workflow when you want to shut down the browser.
