# rive-render

Headless renderer for [Rive](https://rive.app) animations. Generates **PNG screenshots**, **animated GIFs**, and **MP4 videos** from `.riv` files with full support for state machines, linear animations, view model data binding, and referenced assets.

Built on the [Rive PLS Renderer](https://github.com/rive-app/rive-runtime) with Vulkan for GPU-accelerated rendering including feathering and all advanced Rive features. Ships as a C++ CLI binary with a TypeScript/Node.js API.

## Features

- **Screenshot** any frame at a precise timestamp
- **Animated GIF** with palette optimization and Floyd-Steinberg dithering
- **MP4/WebM video** via ffmpeg
- **State machine** and **linear animation** support
- **View model data binding** for dynamic content
- **Referenced asset loading** (images, fonts)
- **CommandQueue/CommandServer** mode matching the Rive app runtime threading pattern
- **Visual regression testing** with jest-image-snapshot
- Works on **macOS** (MoltenVK) and **Linux** (native Vulkan / SwiftShader)

## Quick Start

### TypeScript API

```typescript
import { RiveRenderer } from "@breakaway/rive-render";

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

### CommandQueue/CommandServer Mode

Use the multi-threaded rendering mode that matches the Rive app runtimes (iOS, Android). Handles async asset loading on a background thread:

```typescript
await cli.render({
  rivFile: "animation.riv",
  width: 800,
  height: 600,
  screenshot: { path: "out.png", timestamp: 1.0 },
  useCommandQueue: true,
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
  useCommandQueue?: boolean;
}
```

## Visual Regression Testing

rive-render integrates with [jest-image-snapshot](https://github.com/americanexpress/jest-image-snapshot) for pixel-level visual regression testing of Rive animations.

### Setup

```typescript
import { toMatchImageSnapshot } from "jest-image-snapshot";
import { RiveRenderer } from "@breakaway/rive-render";

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
git clone https://github.com/breakaway/rive-render.git
cd rive-render

# Clone rive-runtime
git clone --depth 1 https://github.com/rive-app/rive-runtime.git deps/rive-runtime

# Build MoltenVK (macOS only, ~5 minutes)
cd deps/rive-runtime/renderer && bash make_moltenvk.sh && cd ../../..

# Build the native binary
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

## Architecture

```
TypeScript API (@breakaway/rive-render)
    |  spawns process, JSON config via stdin
    v
C++ CLI binary (rive_render)
    |
    +-- Rive PLS Renderer (Vulkan -- full feathering support)
    |     +-- VulkanHeadlessFrameSynchronizer (offscreen rendering)
    |     +-- MoltenVK (macOS) / native Vulkan (Linux)
    |
    +-- CommandQueue/CommandServer (optional multi-threaded mode)
    |     +-- Background thread: file loading, asset decoding, state machine
    |     +-- Draw callback: renders on server thread via artboard->draw()
    |
    +-- Output encoders
          +-- PNG (stb_image_write)
          +-- GIF (ffmpeg palettegen/paletteuse)
          +-- MP4/WebM (ffmpeg libx264/libvpx-vp9)
```

### Why not Skia?

The Rive Skia renderer does not support [feathering](https://rive.app/blog/rive-renderer-now-open-source-and-available-on-all-platforms), a key rendering feature. The PLS (Pixel Local Storage) renderer supports all Rive features including feathering, advanced blend modes, and image meshes.

### Rendering Modes

**Direct mode** (default): Loads the `.riv` file, creates artboard and animation instances, advances frame-by-frame, and renders to an offscreen Vulkan surface. Supports both `StateMachine` and `LinearAnimation`.

**CommandQueue/CommandServer mode** (`useCommandQueue: true`): Matches the threading pattern used by the official Rive iOS and Android runtimes. A background server thread owns all Rive objects and processes commands (file loading, asset decoding, state machine advancement). The draw callback executes on the server thread with safe access to the artboard instance. See [Renderer.mm](https://github.com/rive-app/rive-ios/blob/main/Source/Experimental/Renderer/Renderer.mm) for the reference implementation.

## Project Structure

```
rive-render/
+-- native/                     C++ renderer binary
|   +-- src/
|   |   +-- main.cpp            Entry point, JSON config, orchestration
|   |   +-- headless_renderer.* Vulkan headless setup + frame rendering
|   |   +-- queue_renderer.*    CommandQueue/CommandServer mode
|   |   +-- config.*            JSON config parsing
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
|
+-- deps/                       (gitignored)
    +-- rive-runtime/           Rive C++ runtime
```

## License

Apache License 2.0. See [LICENSE](LICENSE).

Rive runtime is licensed separately. See [rive-app/rive-runtime](https://github.com/rive-app/rive-runtime).
