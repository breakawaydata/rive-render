# rive-render

Headless renderer for [Rive](https://rive.app) animations. Generates **PNG screenshots**, **animated GIFs**, and **MP4 videos** from `.riv` files with full support for state machines, linear animations, view model data binding, and referenced assets.

Built on the [Rive PLS Renderer](https://github.com/rive-app/rive-runtime) for GPU-accelerated rendering including feathering and all advanced Rive features — Metal on macOS, Vulkan on Linux. Ships as a C++ CLI binary with a TypeScript/Node.js API.

## Features

- **Screenshot** any frame at a precise timestamp
- **Animated GIF** with palette optimization and Floyd-Steinberg dithering
- **MP4/WebM video** via ffmpeg
- **State machine** and **linear animation** support
- **View model data binding** for dynamic content: strings, numbers, booleans, colors, enums, images, fonts, triggers, lists and artboards from a second `.riv` file
- **Asset overrides**: swap referenced *or* embedded images/fonts; CDN-hosted assets are fetched automatically
- **Rive scripting** (Luau) runs at render time; script logs go to stderr
- **Auto-sizing** to the artboard when width/height are omitted
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
| `render(config, renderOptions?)` | Low-level: full config control |

### Cancelling a Render

`render` and the `screenshot`, `renderGif` and `renderVideo` helpers accept an optional trailing `{ signal }` argument holding an `AbortSignal`. The native process runs in its own process group, and aborting the signal sends `SIGKILL` to that whole group straight away, so the ffmpeg process the binary starts for GIF and video output dies with it. The promise rejects with a `RiveRenderError` whose `exitCode` is `null` once the process has exited, so a retry never overlaps the killed render; an abort while ffmpeg is still being resolved rejects immediately. When the signal's reason is an `Error`, its message is appended to the error message. A signal that is already aborted rejects without starting a process. Use this to enforce a timeout without leaving the native process (hundreds of MiB) running after the caller has given up.

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
      // Bind a `ViewModelInstanceAssetImage` slot to a local image file
      // (PNG/JPEG/WebP). The file is decoded and assigned to the VM
      // image-property slot — distinct from `assets.images`, which is
      // for replacing referenced .riv assets by name.
      teamLogo: { type: "image", value: "/path/to/team-logo.png" },
      // Bind a `ViewModelInstanceAssetFont` slot to a local TTF/OTF file.
      headlineFont: { type: "font", value: "/path/to/Inter-Bold.ttf" },
      // Fire a trigger once, after binding and before the first frame.
      celebrate: { type: "trigger" },
      // Bind a DataList VM property. Each entry instantiates a row VM
      // (defaults to the file's first VM if `viewModel` is omitted) and
      // appends it to the list. Per-row property values can include any
      // PropertyValue, including nested lists and image bindings.
      stats: {
        type: "list",
        value: [
          {
            viewModel: "StatRow",
            properties: {
              label: { type: "string", value: "Points" },
              value: { type: "number", value: 24 },
            },
          },
          {
            viewModel: "StatRow",
            properties: {
              label: { type: "string", value: "Rebounds" },
              value: { type: "number", value: 11 },
            },
          },
        ],
      },
    },
  },
});
```

`viewModel` / `instance` select which view model and named instance to bind
(defaults: the artboard's view model, its default instance). An unknown name
fails the render with `View model not found: …` / `View model instance not found: …`
instead of silently rendering defaults.

### Artboards from a Second File ("Rive on Rive")

A view model property of type artboard can host an artboard from a *second*
`.riv` file, with that artboard's own view model data. Declare the second file
under `extraFiles`, then bind it with an `artboard` property value, keyed by
the artboard property's view model path like every other property type.

`extraFiles` maps an alias of your choosing to a file entry:

| Field | Meaning |
| --- | --- |
| `rivFile` | Path to the extra `.riv` file. |
| `assets` | Optional `images` / `fonts` overrides for this file only. |

An `artboard` property value has these fields:

| Field | Meaning |
| --- | --- |
| `type` | `"artboard"`. |
| `file` | An alias from `extraFiles`. |
| `artboard` | Name of the artboard to take from that file. |
| `viewModel` | Optional view model of the extra file to instantiate. Defaults to the artboard's own view model, then the file's first one. |
| `properties` | Optional property values for that instance, keyed by view model path. Every property type is accepted: strings, images, fonts, lists, triggers and further `artboard` bindings. |

How it behaves:

- The bound artboard always gets a view model instance created from **its own
  file**. Without that instance it would read the main file's view models and
  render empty, so the renderer creates one even when `properties` is omitted.
- The artboard's first state machine runs inside the host and advances with
  the main scene, so animated cards animate.
- Nested triggers fire after the parent view model is bound, like top-level
  ones.
- Each file has its own asset override table. Two files that use the same
  asset name keep their own overrides: the main file's `assets` never reach an
  extra file, and an extra file's `assets` never reach the main file or another
  extra file.
- The `screenshot`, `renderGif` and `renderVideo` helpers take `extraFiles`
  alongside `assets`.

The render fails with a message naming the property when the alias is not in
`extraFiles`, the artboard is not in that file, the `viewModel` is not in that
file, or the main view model has no artboard property at the given path.
Nothing is skipped silently.

### Assets

Supply images and fonts for the file's image/font assets:

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

Keys are matched against each asset in the `.riv`:

- **Unique name** (`name-assetId`, with or without extension, e.g. `"avatar-45020.png"`) —
  replaces the asset whether it is referenced, CDN-hosted or embedded in the file.
- **Bare name** (e.g. `"avatar"`) — replaces referenced / CDN-hosted assets only.

CDN-hosted assets without an override are downloaded while the file loads.

### State Machine Inputs

```typescript
stateMachineInputs: {
  progress: 0.5,     // number input
  isActive: true,    // boolean input
  "Tap": true,       // trigger input: `true` fires it
}
```

### Canvas Size

`width` and `height` are optional in `render()`. Omit both to render at the
artboard's own size; give one and the other follows the artboard's aspect
ratio. The size actually rendered is returned as `result.width` / `result.height`.
(The `screenshot` / `renderGif` / `renderVideo` helpers keep their fixed defaults.)

### Full Configuration

```typescript
interface RiveRenderConfig {
  rivFile: string;
  artboard?: string;
  stateMachine?: string;
  width?: number;   // omit to use the artboard size
  height?: number;
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
  extraFiles?: Record<string, {
    rivFile: string;
    assets?: { images?: Record<string, string>; fonts?: Record<string, string> };
  }>;
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

### Finding ffmpeg

GIF, MP4 and WebM output need an ffmpeg binary. The TypeScript API resolves it in this order: the `ffmpegPath` config option, the `FFMPEG_PATH` environment variable, the first executable `ffmpeg` in a `PATH` directory, a copy previously downloaded into the cache directory, and finally a static build downloaded into that cache directory. The cache directory is `~/.rive-render` unless `RIVE_RENDER_CACHE_DIR` names another one. The download is only attempted when the cache directory is writable; otherwise resolution fails with an error that lists every location it tried.

Neither the resolver nor the native binary needs a shell. The resolver scans `PATH` itself instead of running `which`, and the binary starts ffmpeg with `posix_spawnp` and an explicit argument list instead of `popen`, so rendering works in distroless or hardened images with no `/bin/sh`. ffmpeg's stdout is discarded and its stderr is captured; when ffmpeg cannot be started or exits non-zero, the JSON error names the exit status and quotes the end of ffmpeg's stderr.

### Prerequisites

- macOS or Linux
- Clang (Apple Clang or LLVM)
- Python 3 (for shader compilation)
- glslangValidator (`brew install glslang`)
- macOS: Xcode's Metal Toolchain (`xcodebuild -downloadComponent MetalToolchain`)
- ffmpeg (for GIF/video output)

### Build

```bash
# Clone with submodules
git clone https://github.com/breakawaydata/rive-render.git
cd rive-render

# Clone rive-runtime (pinned ref tracked in native/rive-runtime.version;
# build-native.sh applies any native/rive-runtime-patches/*.patch on top)
git clone --depth 1 --branch "$(cat native/rive-runtime.version)" \
  https://github.com/rive-app/rive-runtime.git deps/rive-runtime

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
3. **Publishes** the `@breakawaydata/rive-render` package to [GitHub Packages](https://npm.pkg.github.com)
4. **Creates a GitHub Release** with the native binaries attached

### Installing from GitHub Packages

Configure npm to use GitHub Packages for the `@breakawaydata` scope:

```bash
echo "@breakawaydata:registry=https://npm.pkg.github.com" >> .npmrc
npm install @breakawaydata/rive-render
```

A `postinstall` script downloads the matching platform binary from the GitHub Release.

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

rive-render delegates all Rive-object lifecycle to Rive's `CommandQueue`/`CommandServer` — the same pattern used by the official Rive iOS and Android runtimes. The `.riv` file is loaded with a custom `FileAssetLoader` that resolves asset overrides and CDN downloads during import, an artboard + state machine (or fallback linear animation) is instantiated, and each frame is advanced and rendered inside a draw callback that runs on the server thread. The client thread only submits commands and collects pixel buffers — no Rive object is ever touched from two threads. See [command_queue.hpp](https://github.com/rive-app/rive-runtime/blob/main/include/rive/command_queue.hpp) for the full API surface.

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
|   |   +-- ffmpeg_process.*    Spawns ffmpeg without a shell
|   +-- premake5.lua            Build configuration
|
+-- ts/                         TypeScript API package
|   +-- src/
|   |   +-- index.ts            Public exports
|   |   +-- rive-render.ts         Core class (spawns binary, manages I/O)
|   |   +-- types.ts            TypeScript interfaces
|   |   +-- binary-resolver.ts  Platform binary resolution
|   |   +-- ffmpeg-resolver.ts  Find ffmpeg (PATH scan, cache, download)
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
|   +-- databind_external_artboard_{main,child}.riv  "Rive on Rive" fixtures
|                                (from rive-app/rive-runtime, MIT)
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
