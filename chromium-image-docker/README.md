# Wootz Chromium Desktop Runtime Image

This directory builds a browser-only Docker image for the Linux desktop Wootz Chromium payload.

The image owns browser lifecycle only:

- start Wootz Chromium
- expose CDP on container port `9222`
- expose VNC/noVNC in headed mode
- keep a reusable browser profile location
- expose basic `status`, `wait`, and `version` commands

## Build Payload

The Chromium release payload must be present in:

```text
build/
  chrome
  chrome_crashpad_handler
  resources.pak
  icudtl.dat
  *.so
  ...
```

This prepared directory currently hardlinks `build/` from `../chromium-image/build/` to avoid another large copy.

## Build

```bash
cd /home/ubuntu/DevJangid/browser-envs/chromium-image-docker
docker buildx build --platform linux/amd64 -t devjangid/wootzapp-chromium-desktop:latest --load .
```

For push:

```bash
docker buildx build --platform linux/amd64 -t devjangid/wootzapp-chromium-desktop:latest --push .
```

## Run Locally

With Compose:

```bash
docker compose up -d
docker compose exec wootzapp-chromium-desktop chromium-runtime-entrypoint status
```

Headed with noVNC:

```bash
CHROMIUM_HEADLESS=0 docker compose up -d
```

Headless:

```bash
docker run -d --rm \
  --name wootzapp-chromium-desktop \
  --shm-size=2g \
  -p 127.0.0.1:9222:9222 \
  devjangid/wootzapp-chromium-desktop:latest
```

Headed with noVNC:

```bash
docker run -d --rm \
  --name wootzapp-chromium-desktop \
  --shm-size=2g \
  -p 127.0.0.1:9222:9222 \
  -p 127.0.0.1:5900:5900 \
  -p 127.0.0.1:6080:6080 \
  -e CHROMIUM_HEADLESS=0 \
  devjangid/wootzapp-chromium-desktop:latest
```

Open:

```text
CDP:   http://127.0.0.1:9222/json/version
noVNC: http://127.0.0.1:6080/vnc.html?host=127.0.0.1&port=6080&autoconnect=1&resize=scale
```

## Runtime Commands

Inside the container:

```bash
chromium-runtime-entrypoint serve
chromium-runtime-entrypoint status
chromium-runtime-entrypoint wait --timeout 30
chromium-runtime-entrypoint version
```
