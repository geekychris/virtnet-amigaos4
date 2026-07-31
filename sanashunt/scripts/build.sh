#!/usr/bin/env bash
# Cross-compile sanashunt.device via the walkero AmigaOS 4 GCC 11 Docker
# image. Wraps `docker exec ... make -C sanashunt/`.
#
# Usage:
#   ./scripts/build.sh          — build device + tests
#   ./scripts/build.sh clean    — nuke build/
set -euo pipefail

# Root the mount at parent virte1000 dir so the Docker container sees
# sanashunt/ as a subdir.
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
IMAGE="walkero/amigagccondocker:os4-gcc11"

case "$(uname -m)" in
  arm64|aarch64) IMAGE="walkero/amigagccondocker:os4-gcc11-arm64" ;;
  x86_64|amd64)  IMAGE="walkero/amigagccondocker:os4-gcc11" ;;
esac

case "${1:-all}" in
  clean)
    rm -rf "$HERE/sanashunt/build"
    exit 0
    ;;
  shell)
    exec docker run --rm -it -v "$HERE:/work" -w /work/sanashunt "$IMAGE" bash
    ;;
esac

exec docker run --rm -v "$HERE:/work" -w /work/sanashunt "$IMAGE" make "$@"
