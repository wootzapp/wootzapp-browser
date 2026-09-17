#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
IMAGE_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
IMAGE_NAME="${CHROMIUM_IMAGE_NAME:-w8-core:chromium-desktop}"
BUILD_DIR="${IMAGE_DIR}/build"

if [ ! -x "${BUILD_DIR}/chrome" ]; then
  echo "Missing executable Chromium payload: ${BUILD_DIR}/chrome" >&2
  echo "Copy your Chromium payload into ${BUILD_DIR}/ before building." >&2
  exit 1
fi

"${BUILD_DIR}/chrome" --version
du -sh "${BUILD_DIR}"

cd "${IMAGE_DIR}"
DOCKER_BUILDKIT=1 docker build \
  --tag "${IMAGE_NAME}" \
  --file Dockerfile \
  .

echo "Built ${IMAGE_NAME}"
