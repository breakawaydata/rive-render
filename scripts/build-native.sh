#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RIVE_RUNTIME="$PROJECT_ROOT/deps/rive-runtime"
BUILD_DIR="$PROJECT_ROOT/native"
CONFIG="${1:-release}"
OS="$(uname -s)"

# --- Download premake5 if needed ---
PREMAKE5="$PROJECT_ROOT/deps/bin/premake5"
if [ ! -x "$PREMAKE5" ]; then
    echo "==> Downloading premake5..."
    mkdir -p "$PROJECT_ROOT/deps/bin"
    ARCH="$(uname -m)"
    if [ "$OS" = "Darwin" ]; then
        PREMAKE_URL="https://github.com/premake/premake-core/releases/download/v5.0.0-beta6/premake-5.0.0-beta6-macosx.tar.gz"
    elif [ "$OS" = "Linux" ]; then
        PREMAKE_URL="https://github.com/premake/premake-core/releases/download/v5.0.0-beta6/premake-5.0.0-beta6-linux.tar.gz"
    else
        echo "Unsupported OS: $OS"
        exit 1
    fi
    curl -L "$PREMAKE_URL" | tar xz -C "$PROJECT_ROOT/deps/bin"
    chmod +x "$PREMAKE5"
fi

echo "==> Using premake5 at: $PREMAKE5"

# --- Build MoltenVK on macOS if not already built ---
if [ "$OS" = "Darwin" ]; then
    MOLTENVK_LIB="$RIVE_RUNTIME/renderer/dependencies/MoltenVK/Package/Release/MoltenVK/dylib/macOS/libMoltenVK.dylib"
    if [ ! -f "$MOLTENVK_LIB" ]; then
        echo "==> Building Rive's custom MoltenVK (required for macOS)..."
        cd "$RIVE_RUNTIME/renderer"
        bash make_moltenvk.sh
        cd "$PROJECT_ROOT"
    fi
    echo "==> MoltenVK available at: $MOLTENVK_LIB"
fi

# --- Build rive-render ---
cd "$BUILD_DIR"

echo "==> Running premake5 (config=$CONFIG, with_vulkan, with-rtti, with-exceptions)..."
"$PREMAKE5" \
    --scripts="$RIVE_RUNTIME/build" \
    --with_vulkan \
    --with-rtti \
    --with-exceptions \
    --with_rive_layout \
    --config="$CONFIG" \
    --out="out/$CONFIG" \
    gmake2

echo "==> Building..."
NPROC=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
make -C "out/$CONFIG" -j"$NPROC" config=default

echo "==> Build complete!"
BINARY="$BUILD_DIR/out/$CONFIG/rive_render"
if [ -f "$BINARY" ]; then
    echo "Binary: $BINARY"
    ls -lh "$BINARY"
    echo ""
    echo "To run, set the MoltenVK library path (macOS):"
    echo "  export DYLD_LIBRARY_PATH=$RIVE_RUNTIME/renderer/dependencies/MoltenVK/Package/Release/MoltenVK/dylib/macOS"
    echo ""
    echo "Example usage:"
    echo "  echo '{\"rivFile\":\"file.riv\",\"width\":800,\"height\":600,\"screenshot\":{\"path\":\"out.png\"}}' | $BINARY"
else
    echo "ERROR: Binary not found at $BINARY"
    exit 1
fi
