#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
IMAGE_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd -- "${IMAGE_DIR}/.." && pwd)"
AGENT_BROWSER_DIR="${REPO_ROOT}/agent-browser"
IMAGE_NAME="${CHROMIUM_IMAGE_NAME:-w8-core:chromium-desktop}"
BUILD_DIR="${IMAGE_DIR}/build"
IMAGE_VERSION="${IMAGE_VERSION:-dev}"
VCS_REF="${IMAGE_VCS_REF:-$(git -C "${REPO_ROOT}" rev-parse HEAD)}"

if [ ! -x "${BUILD_DIR}/chrome" ]; then
  echo "Missing executable Chromium payload: ${BUILD_DIR}/chrome" >&2
  echo "Copy your Chromium payload into ${BUILD_DIR}/ before building." >&2
  exit 1
fi

for required in package.json package-lock.json; do
  if [ ! -f "${AGENT_BROWSER_DIR}/${required}" ]; then
    echo "Missing agent-browser build input: ${AGENT_BROWSER_DIR}/${required}" >&2
    exit 1
  fi
done

"${BUILD_DIR}/chrome" --version
du -sh "${BUILD_DIR}"

cd "${IMAGE_DIR}"
docker buildx build \
  --load \
  --build-context "agent_browser=${AGENT_BROWSER_DIR}" \
  --build-arg "IMAGE_VERSION=${IMAGE_VERSION}" \
  --build-arg "VCS_REF=${VCS_REF}" \
  --tag "${IMAGE_NAME}" \
  --file Dockerfile \
  .

echo "Built ${IMAGE_NAME}"
