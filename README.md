# rive-render

Headless renderer for [Rive](https://rive.app) animations. Generates **PNG screenshots**, **animated GIFs**, and **MP4 videos** from `.riv` files with full support for state machines, linear animations, view model data binding, and referenced assets.

Built on the [Rive PLS Renderer](https://github.com/rive-app/rive-runtime) for GPU-accelerated rendering including feathering and all advanced Rive features — Metal on macOS, Vulkan on Linux. Ships as a C++ CLI binary with a TypeScript/Node.js API.

## Features

- **Screenshot** any frame at a precise timestamp
- **Animated GIF** with palette optimization and Floyd-Steinberg dithering
- **MP4/WebM video** via ffmpeg
- **State machine** and **linear animation** support
- **View model data binding** for dynamic content
- **Referenced asset loading** (images, fonts)
- **Multi-threaded rendering** via Rive's `CommandQueue`/`CommandServer` (matches the Rive iOS/Android app runtimes)
- **Visual regression testing** with jest-image-snapshot
- Runs natively on **macOS** (Metal) and **Linux** (Vulkan, with optional bundled SwiftShader for headless/CI environments)

## Quick Start

### TypeScript API

```typescript
import { RiveRenderer } from "@breakawaydata/rive-render";

const cli = new RiveRenderer();

// Screenshot at a specific timestamp
await cli.screenshot("animation.riv", {
  outputPath: "frame.png",
  width: 800,
  height: 600,
  timestamp: 1.5,
});

// Animated GIF
await cli.renderGif("animation.riv", {
  outputPath: "animation.gif",
  width: 400,
  height: 400,
  fps: 30,
  duration: 3.0,
});

// MP4 video
await cli.renderVideo("animation.riv", {
  outputPath: "video.mp4",
  width: 1920,
  height: 1080,
  fps: 60,
  duration: 5.0,
});
```

### CLI Binary

The binary reads JSON configuration from stdin and writes a JSON result to stdout:

```bash
echo '{
  "rivFile": "animation.riv",
  "width": 800,
  "height": 600,
  "screenshot": {
    "path": "output.png",
    "timestamp": 1.0
  }
}' | ./rive_render
```

## API Reference

### `RiveRenderer`

```typescript
const cli = new RiveRenderer(options?: { binaryPath?: string });
```

| Method | Description |
|--------|-------------|
| `screenshot(rivFile, options)` | Capture a single frame as PNG |
| `renderGif(rivFile, options)` | Render an animated GIF |
| `renderVideo(rivFile, options)` | Render an MP4 or WebM video |
| `render(config)` | Low-level: full config control |

### Screenshot Options

```typescript
await cli.screenshot("file.riv", {
  outputPath: "out.png",       // required
  width: 800,                  // default: 800
  height: 600,                 // default: 600
  timestamp: 2.5,              // seconds, default: 0
  artboard: "MyArtboard",      // optional, uses default
  stateMachine: "MySM",        // optional, uses default
  viewModelData: { ... },      // optional
  assets: { ... },             // optional
});
```

### GIF Options

```typescript
await cli.renderGif("file.riv", {
  outputPath: "out.gif",       // required
  duration: 3.0,               // seconds, required
  width: 400,                  // default: 800
  height: 400,                 // default: 600
  fps: 30,                     // default: 30
  artboard: "MyArtboard",      // optional
  viewModelData: { ... },      // optional
  assets: { ... },             // optional
});
```

### Video Options

```typescript
await cli.renderVideo("file.riv", {
  outputPath: "out.mp4",       // required
  duration: 5.0,               // seconds, required
  format: "mp4",               // "mp4" | "webm", default: "mp4"
  width: 1920,                 // default: 1920
  height: 1080,                // default: 1080
  fps: 60,                     // default: 60
  artboard: "MyArtboard",      // optional
  viewModelData: { ... },      // optional
  assets: { ... },             // optional
});
```

### View Model Data Binding

Pass dynamic data to Rive view models:

```typescript
await cli.render({
  rivFile: "dashboard.riv",
  width: 800,
  height: 600,
  screenshot: { path: "out.png", timestamp: 0 },
  viewModelData: {
    properties: {
      title: { type: "string", value: "Hello World" },
      progress: { type: "number", value: 0.75 },
      isActive: { type: "boolean", value: true },
      primaryColor: { type: "color", value: "#FF5500" },
    },
  },
});
```

### Referenced Assets

Load external images and fonts:

```typescript
await cli.render({
  rivFile: "design.riv",
  width: 800,
  height: 600,
  screenshot: { path: "out.png" },
  assets: {
    images: {
      "avatar.png": "/path/to/avatar.png",
      "background.jpg": "/path/to/bg.jpg",
    },
    fonts: {
      "Inter": "/path/to/Inter.ttf",
    },
  },
});
```

### Full Configuration

```typescript
interface RiveRenderConfig {
  rivFile: string;
  artboard?: string;
  stateMachine?: string;
  width: number;
  height: number;
  screenshot?: { path: string; timestamp?: number };
  output?: {
    format: "png" | "gif" | "mp4" | "webm";
    path: string;
    fps?: number;
    duration: number;
    quality?: number;
  };
  viewModelData?: {
    viewModel?: string;
    instance?: string;
    properties: Record<string, PropertyValue>;
  };
  assets?: {
    images?: Record<string, string>;
    fonts?: Record<string, string>;
  };
  stateMachineInputs?: Record<string, boolean | number>;
  ffmpegPath?: string;
}
```

## Visual Regression Testing

rive-render integrates with [jest-image-snapshot](https://github.com/americanexpress/jest-image-snapshot) for pixel-level visual regression testing of Rive animations.

### Setup

```typescript
import { toMatchImageSnapshot } from "jest-image-snapshot";
import { RiveRenderer } from "@breakawaydata/rive-render";

expect.extend({ toMatchImageSnapshot });

const cli = new RiveRenderer();

// Render to a PNG buffer
async function renderFrame(rivFile: string, timestamp: number): Promise<Buffer> {
  const tmp = `/tmp/snap-${Date.now()}.png`;
  await cli.render({
    rivFile,
    width: 400,
    height: 400,
    screenshot: { path: tmp, timestamp },
  });
  const buf = readFileSync(tmp);
  unlinkSync(tmp);
  return buf;
}

// Snapshot test
it("matches reference", async () => {
  const image = await renderFrame("animation.riv", 1.0);
  expect(image).toMatchImageSnapshot({
    failureThreshold: 0.001,
    failureThresholdType: "percent",
  });
});
```

### GIF/MP4 File Snapshots

Full file comparison using SHA-256 hashes ensures byte-identical output:

```typescript
import { createHash } from "crypto";

function sha256(buf: Buffer): string {
  return createHash("sha256").update(buf).digest("hex");
}

it("GIF matches reference", async () => {
  const result = await cli.renderGif("animation.riv", {
    outputPath: "/tmp/test.gif",
    fps: 10,
    duration: 1.0,
  });
  // Compare against committed reference file
  const actual = readFileSync("/tmp/test.gif");
  const reference = readFileSync("__file_snapshots__/animation-1s.gif");
  expect(sha256(actual)).toBe(sha256(reference));
});
```

### Updating Snapshots

```bash
# Update all snapshots after intentional visual changes
cd ts && npm run test:update
```

## Building from Source

### Prerequisites

- macOS or Linux
- Clang (Apple Clang or LLVM)
- Python 3 (for shader compilation)
- glslangValidator (`brew install glslang`)
- ffmpeg (for GIF/video output)

### Build

```bash
# Clone with submodules
git clone https://github.com/breakawaydata/rive-render.git
cd rive-render

# Clone rive-runtime
git clone --depth 1 https://github.com/rive-app/rive-runtime.git deps/rive-runtime

# Build the native binary
# (on Linux this also builds SwiftShader the first time, ~15-20 min;
#  set RIVE_RENDER_SKIP_SWIFTSHADER=1 to skip)
bash scripts/build-native.sh

# Install TypeScript dependencies
cd ts && npm install && npm run build
```

### Running Tests

```bash
cd ts

# Run all tests
npm test

# Update snapshots after intentional changes
npm run test:update
```

## CI / CD

### Pull Request Checks

Every PR runs three parallel jobs on Ubuntu:

- **Lint TypeScript** — ESLint with typescript-eslint
- **Lint C++** — clang-format 18.1.8 check on `native/src/`
- **Build & Test** — Build the native binary, compile TypeScript, run Jest tests

The native build output is cached by source hash, so PRs that only change TypeScript skip the ~10 minute C++ compilation.

### Releasing

Releases are triggered by pushing a version tag. The CI workflow automatically stamps all package versions from the tag — no manual version bumps needed:

```bash
git tag v0.2.0
git push origin v0.2.0
```

This triggers the release workflow which:

1. **Builds** native binaries on 4 platforms (darwin-arm64, darwin-x64, linux-x64, linux-arm64)
2. **Tests** using the linux-x64 binary
3. **Publishes** platform-specific npm packages (`@breakawaydata/rive-render-darwin-arm64`, etc.) and the main `@breakawaydata/rive-render` package to [GitHub Packages](https://npm.pkg.github.com)
4. **Creates a GitHub Release** with the native binaries attached

### Installing from GitHub Packages

Configure npm to use GitHub Packages for the `@breakawaydata` scope:

```bash
echo "@breakawaydata:registry=https://npm.pkg.github.com" >> .npmrc
npm install @breakawaydata/rive-render
```

The correct platform-specific binary is installed automatically via `optionalDependencies`.

### Updating Test Snapshots

GIF/MP4 file snapshots are platform-specific. To update after intentional rendering changes:

```bash
cd ts
npm run test:update
```

If updating on macOS, the CI (Linux) snapshots will differ. Push your changes — CI will fail, generate updated snapshots as an artifact, which you can download and commit:

```bash
# After CI fails, download the updated snapshots from the failed run
gh run download <RUN_ID> -n updated-snapshots -D /tmp/updated-snapshots
cp /tmp/updated-snapshots/__file_snapshots__/*.gif ts/src/test/__file_snapshots__/
cp /tmp/updated-snapshots/__file_snapshots__/*.mp4 ts/src/test/__file_snapshots__/
cp /tmp/updated-snapshots/__image_snapshots__/*.png ts/src/test/__image_snapshots__/
git add ts/src/test && git commit -m "fix: update snapshots for Linux CI"
git push
```

## Architecture

```
TypeScript API (@breakawaydata/rive-render)
    |  spawns process, JSON config via stdin
    v
C++ CLI binary (rive_render)
    |
    +-- CommandQueue / CommandServer
    |     |  Client thread: config parsing, command submission, frame
    |     |                 collection, output encoding
    |     +- Background server thread: owns all Rive objects (file,
    |        artboard, state machine, view model, assets). Processes
    |        commands FIFO and executes per-frame draw callbacks.
    |
    +-- Rive PLS Renderer -- full feathering support
    |     +-- macOS: Metal backend
    |     |     +-- offscreen MTLTexture + MTLBuffer blit readback
    |     +-- Linux: Vulkan backend
    |           +-- VulkanHeadlessFrameSynchronizer (offscreen rendering)
    |           +-- real GPU driver, or bundled SwiftShader via `"swiftshader":true`
    |
    +-- Output encoders
          +-- PNG (stb_image_write)
          +-- GIF (ffmpeg palettegen/paletteuse)
          +-- MP4/WebM (ffmpeg libx264/libvpx-vp9)
```

### Why not Skia?

The Rive Skia renderer does not support [feathering](https://rive.app/blog/rive-renderer-now-open-source-and-available-on-all-platforms), a key rendering feature. The PLS (Pixel Local Storage) renderer supports all Rive features including feathering, advanced blend modes, and image meshes.

### Rendering pipeline

rive-render delegates all Rive-object lifecycle to Rive's `CommandQueue`/`CommandServer` — the same pattern used by the official Rive iOS and Android runtimes. Referenced assets are decoded and globally registered, the `.riv` file is loaded, an artboard + state machine (or fallback linear animation) is instantiated, and each frame is advanced and rendered inside a draw callback that runs on the server thread. The client thread only submits commands and collects pixel buffers — no Rive object is ever touched from two threads. See [command_queue.hpp](https://github.com/rive-app/rive-runtime/blob/main/include/rive/command_queue.hpp) for the full API surface.

## Project Structure

```
rive-render/
+-- native/                     C++ renderer binary
|   +-- src/
|   |   +-- main.cpp                     Entry point, JSON config, orchestration
|   |   +-- queue_renderer.*             CommandQueue driver: assets, artboard, frames
|   |   +-- headless_renderer.hpp        Backend-agnostic offscreen renderer API
|   |   +-- headless_renderer_metal.mm   macOS Metal backend
|   |   +-- headless_renderer_vulkan.cpp Linux Vulkan / SwiftShader backend
|   |   +-- config.*                     JSON config parsing
|   |   +-- output_png.*        PNG encoding (stb_image_write)
|   |   +-- output_gif.*        GIF via ffmpeg
|   |   +-- output_video.*      MP4/WebM via ffmpeg
|   +-- premake5.lua            Build configuration
|
+-- ts/                         TypeScript API package
|   +-- src/
|   |   +-- index.ts            Public exports
|   |   +-- rive-render.ts         Core class (spawns binary, manages I/O)
|   |   +-- types.ts            TypeScript interfaces
|   |   +-- binary-resolver.ts  Platform binary resolution
|   |   +-- ffmpeg-resolver.ts  Auto-download ffmpeg
|   |   +-- test/
|   |       +-- snapshot.test.ts            All tests
|   |       +-- __image_snapshots__/        Reference PNGs (committed)
|   |       +-- __file_snapshots__/         Reference GIFs/MP4s (committed)
|   +-- jest.config.js
|   +-- package.json
|
+-- test/fixtures/              Test .riv files
|   +-- basketball.riv          LinearAnimation test fixture
|   +-- teststatemachine.riv    StateMachine test fixture
|
+-- scripts/
|   +-- build-native.sh         Full build script
|   +-- version.sh              Stamp version across all packages
|
+-- npm/                        Platform-specific binary packages
|   +-- darwin-arm64/
|   +-- darwin-x64/
|   +-- linux-x64/
|   +-- linux-arm64/
|
+-- .github/workflows/
|   +-- ci.yml                  PR checks (lint, build, test)
|   +-- release.yml             Release (build, publish, GitHub Release)
|
+-- deps/                       (gitignored)
    +-- rive-runtime/           Rive C++ runtime
```

## License

Apache License 2.0. See [LICENSE](LICENSE).

Rive runtime is licensed separately. See [rive-app/rive-runtime](https://github.com/rive-app/rive-runtime).
