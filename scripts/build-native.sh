#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RIVE_RUNTIME="$PROJECT_ROOT/deps/rive-runtime"
BUILD_DIR="$PROJECT_ROOT/native"
CONFIG="${1:-release}"
TARGET_ARCH="${2:-}"
OS="$(uname -s)"

# --- Download premake5 if needed ---
PREMAKE5="$PROJECT_ROOT/deps/bin/premake5"
if [ ! -x "$PREMAKE5" ]; then
    echo "==> Downloading premake5..."
    mkdir -p "$PROJECT_ROOT/deps/bin"
    HOST_ARCH="$(uname -m)"
    if [ "$OS" = "Darwin" ]; then
        PREMAKE_URL="https://github.com/premake/premake-core/releases/download/v5.0.0-beta6/premake-5.0.0-beta6-macosx.tar.gz"
    elif [ "$OS" = "Linux" ] && [ "$HOST_ARCH" = "x86_64" ]; then
        PREMAKE_URL="https://github.com/premake/premake-core/releases/download/v5.0.0-beta6/premake-5.0.0-beta6-linux.tar.gz"
    elif [ "$OS" = "Linux" ] && [ "$HOST_ARCH" = "aarch64" ]; then
        echo "==> No prebuilt premake5 for Linux ARM64, building from source..."
        PREMAKE_SRC="$PROJECT_ROOT/deps/premake-src"
        curl -L "https://github.com/premake/premake-core/releases/download/v5.0.0-beta6/premake-5.0.0-beta6-src.zip" -o /tmp/premake-src.zip
        unzip -q /tmp/premake-src.zip -d "$PREMAKE_SRC"
        make -C "$PREMAKE_SRC/premake-5.0.0-beta6-src" -f Bootstrap.mak linux
        cp "$PREMAKE_SRC/premake-5.0.0-beta6-src/bin/release/premake5" "$PREMAKE5"
        chmod +x "$PREMAKE5"
        rm -rf "$PREMAKE_SRC" /tmp/premake-src.zip
    else
        echo "Unsupported OS/arch: $OS/$HOST_ARCH"
        exit 1
    fi
    if [ -z "${PREMAKE_URL:-}" ]; then
        : # Already built from source
    else
        curl -L "$PREMAKE_URL" | tar xz -C "$PROJECT_ROOT/deps/bin"
        chmod +x "$PREMAKE5"
    fi
fi

echo "==> Using premake5 at: $PREMAKE5"

# --- Patch rive-runtime's Vulkan library loader ---
# Add more candidate paths so MoltenVK is found when installed via Homebrew
# or bundled next to the executable.
VULKAN_LIB_CPP="$RIVE_RUNTIME/renderer/rive_vk_bootstrap/src/vulkan_library.cpp"
if [ -f "$VULKAN_LIB_CPP" ] && ! grep -q "RIVE_RENDER_MVK_PATHS" "$VULKAN_LIB_CPP"; then
    echo "==> Patching rive-runtime vulkan_library.cpp with extra MoltenVK candidate paths..."
    python3 - "$VULKAN_LIB_CPP" <<'PYEOF'
import sys
path = sys.argv[1]
with open(path) as f:
    content = f.read()

# Insert extra candidates after "libMoltenVK.dylib",
old = '        "libMoltenVK.dylib",\n'
new = (
    '        "libMoltenVK.dylib",\n'
    '        // RIVE_RENDER_MVK_PATHS: extra candidates for rive-render\n'
    '        "/opt/homebrew/lib/libMoltenVK.dylib", // ARM macOS Homebrew\n'
    '        "/usr/local/lib/libMoltenVK.dylib",    // Intel macOS Homebrew\n'
)
if old not in content:
    raise SystemExit("ERROR: Could not find patch target in vulkan_library.cpp")
content = content.replace(old, new, 1)
with open(path, 'w') as f:
    f.write(content)
print("Patched vulkan_library.cpp")
PYEOF
fi

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

PREMAKE_ARCH_FLAG=""
if [ -n "$TARGET_ARCH" ]; then
    PREMAKE_ARCH_FLAG="--arch=$TARGET_ARCH"
    echo "==> Cross-compiling for architecture: $TARGET_ARCH"
fi

echo "==> Running premake5 (config=$CONFIG, with_vulkan, with-rtti, with-exceptions)..."
"$PREMAKE5" \
    --scripts="$RIVE_RUNTIME/build" \
    --with_vulkan \
    --with-rtti \
    --with-exceptions \
    --with_rive_layout \
    --config="$CONFIG" \
    --out="out/$CONFIG" \
    $PREMAKE_ARCH_FLAG \
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
    echo "Example usage:"
    echo "  echo '{\"rivFile\":\"file.riv\",\"width\":800,\"height\":600,\"screenshot\":{\"path\":\"out.png\"}}' | $BINARY"
else
    echo "ERROR: Binary not found at $BINARY"
    exit 1
fi
