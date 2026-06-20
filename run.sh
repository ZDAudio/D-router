#!/usr/bin/env bash
# dcorerouter — build + launch helper.
#
# Usage:
#   ./run.sh                build (incremental) and launch
#   ./run.sh --no-build     just launch the existing build
#   ./run.sh --clean        wipe build/ and full rebuild before launching
#   ./run.sh --debug        build Debug configuration and launch
#   ./run.sh --kill         only kill any running instance, do nothing else

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
CONFIG="Release"
# JUCE PRODUCT_NAME — names both the .app bundle and the running process.
PRODUCT_NAME="D-Router"

CMAKE="$(command -v cmake || echo /opt/homebrew/bin/cmake)"
NINJA="$(command -v ninja || echo /opt/homebrew/bin/ninja)"

DO_BUILD=1
DO_CLEAN=0

for arg in "$@"; do
    case "$arg" in
        --no-build) DO_BUILD=0 ;;
        --clean)    DO_CLEAN=1 ;;
        --debug)    CONFIG="Debug"; BUILD_DIR="$PROJECT_DIR/build-debug" ;;
        --kill)
            killall "$PRODUCT_NAME" 2>/dev/null || true
            echo "[dcorerouter] killed."
            exit 0
            ;;
        -h|--help)
            sed -n '2,9p' "$0" | sed 's/^# //; s/^#//'
            exit 0
            ;;
        *)
            echo "Unknown option: $arg" >&2
            exit 2
            ;;
    esac
done

APP_BUNDLE="$BUILD_DIR/dcorerouter_artefacts/$CONFIG/$PRODUCT_NAME.app"

# 1. Kill any running instance so we relaunch cleanly.
killall "$PRODUCT_NAME" 2>/dev/null || true
sleep 0.3

# 2. Optional clean.
if [[ $DO_CLEAN -eq 1 ]]; then
    echo "[dcorerouter] cleaning $BUILD_DIR..."
    rm -rf "$BUILD_DIR"
fi

# 3. Build (configure if needed, then ninja).
if [[ $DO_BUILD -eq 1 ]]; then
    if [[ ! -d "$BUILD_DIR" ]]; then
        echo "[dcorerouter] configuring CMake ($CONFIG) in $BUILD_DIR..."
        "$CMAKE" -B "$BUILD_DIR" -G Ninja \
                 -DCMAKE_BUILD_TYPE="$CONFIG" \
                 -DCMAKE_MAKE_PROGRAM="$NINJA"
    fi
    echo "[dcorerouter] building..."
    "$CMAKE" --build "$BUILD_DIR" -j 8
fi

# 4. Launch.
if [[ ! -d "$APP_BUNDLE" ]]; then
    echo "[dcorerouter] app bundle not found: $APP_BUNDLE" >&2
    echo "             try running without --no-build, or with --clean." >&2
    exit 1
fi

echo "[dcorerouter] launching $APP_BUNDLE"
open "$APP_BUNDLE"
