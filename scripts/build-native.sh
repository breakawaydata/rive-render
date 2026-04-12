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

# --- Build SwiftShader on Linux so it ships next to the binary ---
# Set RIVE_RENDER_SKIP_SWIFTSHADER=1 to skip. CI uses this in pull-
# request workflows so the main build stays fast; the release workflow
# actually builds SwiftShader from source and bundles it next to the
# binary.
#
# We don't call rive-runtime's `make_swiftshader.sh` because that
# script runs `cmake --build . --parallel` with no job count, which
# CMake translates to bare `make -j` (unlimited parallelism). On a
# 16 GB runner that OOM-kills cc1plus halfway through llvm-10. Build
# in place so we can pass an explicit -j and cap memory pressure.
SWIFTSHADER_LIB=""
SWIFTSHADER_ICD=""
if [ "$OS" = "Linux" ] && [ -z "${RIVE_RENDER_SKIP_SWIFTSHADER:-}" ]; then
    SS_SRC="$RIVE_RUNTIME/renderer/dependencies/swiftshader"
    SS_BUILD="$SS_SRC/build"
    SWIFTSHADER_PARALLEL="${SWIFTSHADER_PARALLEL:-2}"
    if [ ! -f "$SS_BUILD/Linux/libvk_swiftshader.so" ] && \
       [ ! -f "$SS_BUILD/libvk_swiftshader.so" ]; then
        echo "==> Building SwiftShader (software Vulkan for Linux, -j$SWIFTSHADER_PARALLEL)..."
        mkdir -p "$RIVE_RUNTIME/renderer/dependencies"
        if [ ! -d "$SS_SRC" ]; then
            git clone --depth 1 https://github.com/google/swiftshader.git "$SS_SRC"
        fi
        mkdir -p "$SS_BUILD"
        (
            cd "$SS_BUILD"
            cmake ..
            cmake --build . --parallel "$SWIFTSHADER_PARALLEL"
        )
    fi
    # SwiftShader's CMake puts output under either Linux/ or the build
    # root depending on the version; prefer the platform subdir.
    if [ -f "$SS_BUILD/Linux/libvk_swiftshader.so" ]; then
        SWIFTSHADER_LIB="$SS_BUILD/Linux/libvk_swiftshader.so"
        SWIFTSHADER_ICD="$SS_BUILD/Linux/vk_swiftshader_icd.json"
    elif [ -f "$SS_BUILD/libvk_swiftshader.so" ]; then
        SWIFTSHADER_LIB="$SS_BUILD/libvk_swiftshader.so"
        SWIFTSHADER_ICD="$SS_BUILD/vk_swiftshader_icd.json"
    else
        echo "WARN: SwiftShader build completed but libvk_swiftshader.so not found under $SS_BUILD"
    fi
    if [ -n "$SWIFTSHADER_LIB" ]; then
        echo "==> SwiftShader available at: $SWIFTSHADER_LIB"
    fi
fi

# --- Build rive-render ---
cd "$BUILD_DIR"

PREMAKE_VULKAN_FLAG=""
if [ "$OS" = "Linux" ]; then
    PREMAKE_VULKAN_FLAG="--with_vulkan"
fi

PREMAKE_ARCH_FLAG=""
if [ -n "$TARGET_ARCH" ]; then
    PREMAKE_ARCH_FLAG="--arch=$TARGET_ARCH"
    echo "==> Cross-compiling for architecture: $TARGET_ARCH"
fi

echo "==> Running premake5 (config=$CONFIG)..."
"$PREMAKE5" \
    --scripts="$RIVE_RUNTIME/build" \
    $PREMAKE_VULKAN_FLAG \
    --with-rtti \
    --with-exceptions \
    --with_rive_text \
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

    # Stage SwiftShader artifacts next to the binary so the runtime
    # `--swiftshader` flag (which reads them from /proc/self/exe's
    # directory) can pick them up.
    if [ "$OS" = "Linux" ] && [ -n "$SWIFTSHADER_LIB" ] && [ -f "$SWIFTSHADER_LIB" ]; then
        BIN_DIR="$(dirname "$BINARY")"
        cp "$SWIFTSHADER_LIB" "$BIN_DIR/"
        cp "$SWIFTSHADER_ICD" "$BIN_DIR/"
        # Rewrite the ICD's library_path to a relative reference so the
        # bundle is relocatable (SwiftShader's cmake bakes an absolute
        # path by default).
        python3 - "$BIN_DIR/vk_swiftshader_icd.json" <<'PYEOF'
import json, sys
p = sys.argv[1]
with open(p) as f:
    data = json.load(f)
data["ICD"]["library_path"] = "./libvk_swiftshader.so"
with open(p, "w") as f:
    json.dump(data, f, indent=4)
print(f"Rewrote {p} library_path -> ./libvk_swiftshader.so")
PYEOF
        echo "==> Bundled SwiftShader: $BIN_DIR/{libvk_swiftshader.so,vk_swiftshader_icd.json}"
    fi

    echo ""
    echo "Example usage:"
    echo "  echo '{\"rivFile\":\"file.riv\",\"width\":800,\"height\":600,\"screenshot\":{\"path\":\"out.png\"}}' | $BINARY"
else
    echo "ERROR: Binary not found at $BINARY"
    exit 1
fi
