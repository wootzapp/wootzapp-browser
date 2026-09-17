# syntax=docker/dockerfile:1.7
FROM node:24-bookworm-slim AS node-runtime

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8
ENV LC_ALL=C.UTF-8
ENV CHROMIUM_BIN=/opt/wootzapp/chrome
ENV CHROMIUM_RUNTIME_DIR=/tmp/chromium-runtime
ENV CDP_BIND_HOST=0.0.0.0
ENV CDP_PROBE_HOST=127.0.0.1
ENV CDP_PORT=9222
ENV CHROMIUM_CDP_PORT=9223
ENV CHROMIUM_HEADLESS=1
ENV CHROMIUM_RESET_PROFILE=1
ENV CHROMIUM_NO_SANDBOX=1
ENV AUTHORING_MODE=off
ENV AUTHORING_SESSION=default
ENV DISPLAY=:0
ENV VNC_PORT=5900
ENV NOVNC_PORT=6080
ENV VNC_SCREEN=1366x900x24
ENV AGENT_BROWSER_CDP_TARGET=http://127.0.0.1:9222

ARG WOOTZ_UID=1200
ARG WOOTZ_GID=1200
ARG IMAGE_VERSION=dev
ARG VCS_REF=unknown

LABEL org.opencontainers.image.title="w8-core Chromium desktop" \
      org.opencontainers.image.description="Wootz desktop browser with ChromiumRL CDP and agent-browser" \
      org.opencontainers.image.source="https://github.com/wootzapp/wootzapp-browser" \
      org.opencontainers.image.version="${IMAGE_VERSION}" \
      org.opencontainers.image.revision="${VCS_REF}"

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt-get update \
    && apt-get install -y --no-install-recommends \
      ca-certificates \
      fontconfig \
      fonts-liberation \
      libasound2t64 \
      libatk-bridge2.0-0 \
      libatk1.0-0 \
      libatspi2.0-0 \
      libcairo2 \
      libcups2 \
      libcurl4 \
      libdbus-1-3 \
      libdrm2 \
      libexpat1 \
      libgbm1 \
      libglib2.0-0 \
      libgtk-3-0 \
      libnspr4 \
      libnss3 \
      libpango-1.0-0 \
      libpangocairo-1.0-0 \
      libx11-6 \
      libx11-xcb1 \
      libxcb1 \
      libxcomposite1 \
      libxdamage1 \
      libxext6 \
      libxfixes3 \
      libxkbcommon0 \
      libxrandr2 \
      libxrender1 \
      libxshmfence1 \
      libxss1 \
      libxtst6 \
      novnc \
      python3 \
      socat \
      util-linux \
      websockify \
      x11vnc \
      xvfb \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

RUN groupadd --gid "${WOOTZ_GID}" wootz \
    && useradd --uid "${WOOTZ_UID}" --gid wootz --create-home --home-dir /home/wootz --shell /bin/bash wootz \
    && mkdir -p /opt/wootzapp /opt/chromium-runtime /home/wootz/wootz-browser-sessions /tmp/chromium-runtime /tmp/.X11-unix \
    && chmod 1777 /tmp/.X11-unix \
    && chown -R wootz:wootz /opt/wootzapp /opt/chromium-runtime /home/wootz /tmp/chromium-runtime

COPY --chown=wootz:wootz build/ /opt/wootzapp/
COPY --chmod=0755 image/ /

COPY --from=node-runtime /usr/local/bin/node /usr/local/bin/node
COPY --from=node-runtime /usr/local/lib/node_modules/ /usr/local/lib/node_modules/

RUN ln -sf /usr/local/lib/node_modules/npm/bin/npm-cli.js /usr/local/bin/npm \
    && ln -sf /usr/local/lib/node_modules/npm/bin/npx-cli.js /usr/local/bin/npx \
    && node --version \
    && npm --version

WORKDIR /opt/agent-browser
COPY --from=agent_browser package.json package-lock.json ./
RUN --mount=type=cache,target=/root/.npm,sharing=locked \
    npm ci --omit=dev \
    && ln -sf /opt/agent-browser/node_modules/.bin/agent-browser /usr/local/bin/agent-browser-core \
    && agent-browser-core --version
COPY --from=agent_browser bin/ ./bin/
COPY --from=agent_browser lib/ ./lib/
COPY --from=agent_browser prompts/ ./prompts/
RUN chmod 0755 /opt/agent-browser/bin/agent-browser-desktop.mjs \
    && ln -sf /opt/agent-browser/bin/agent-browser-desktop.mjs /usr/local/bin/agent-browser \
    && agent-browser --version

WORKDIR /home/wootz
EXPOSE 9222 5900 6080

HEALTHCHECK --interval=15s --timeout=5s --start-period=20s --retries=4 \
  CMD chromium-runtime-entrypoint status --quiet || exit 1

ENTRYPOINT ["/usr/local/bin/chromium-runtime-entrypoint"]
CMD ["serve"]
