#!/usr/bin/env bash
set -euo pipefail

CONTAINER_NAME="${CHROMIUM_CONTAINER_NAME:-w8-core}"

if docker ps -a --format '{{.Names}}' | grep -Fxq "${CONTAINER_NAME}"; then
  docker rm -f "${CONTAINER_NAME}"
else
  echo "${CONTAINER_NAME} is not present"
fi
