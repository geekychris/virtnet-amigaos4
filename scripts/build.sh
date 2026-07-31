#!/usr/bin/env bash
# Cross-compile virte1000.device (and tests/testopen) for AmigaOS 4.1 PPC
# via the walkero AmigaOS 4 GCC 11 Docker image. Mirrors the pattern
# from ../python-amigaos4/build.sh.
#
# Usage:
#   ./scripts/build.sh          — configure environment + `make` inside container
#   ./scripts/build.sh clean    — nuke build/
#   ./scripts/build.sh shell    — drop into an interactive Docker shell
#   ./scripts/build.sh <targ>   — pass through to make, e.g. `./scripts/build.sh test`
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="walkero/amigagccondocker:os4-gcc11"

# Force the correct image variant for the host arch. Apple Silicon needs
# the arm64 tag; the plain :os4-gcc11 tag on Docker Hub is x86_64 and
# runs slowly under Rosetta emulation.
ARCH="$(uname -m)"
case "$ARCH" in
  arm64|aarch64) IMAGE="walkero/amigagccondocker:os4-gcc11-arm64" ;;
  x86_64|amd64)  IMAGE="walkero/amigagccondocker:os4-gcc11" ;;
esac

case "${1:-all}" in
  clean)
    rm -rf "$HERE/build"
    exit 0
    ;;
  shell)
    exec docker run --rm -it -v "$HERE:/work" -w /work "$IMAGE" bash
    ;;
esac

exec docker run --rm -v "$HERE:/work" -w /work "$IMAGE" make "$@"
