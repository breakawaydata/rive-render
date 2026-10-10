export interface ScreenshotOptions {
  /** Output PNG path */
  path: string;
  /** Timestamp in seconds to capture (default: 0) */
  timestamp?: number;
}

export interface OutputConfig {
  /** Output format */
  format: "png" | "gif" | "mp4" | "webm";
  /** Output file path */
  path: string;
  /** Frames per second (default: 30) */
  fps?: number;
  /** Duration in seconds */
  duration: number;
  /** Quality 1-100 (default: 90) */
  quality?: number;
  /**
   * Which H.264 encoder produces an mp4 (ignored for other formats).
   * - `"auto"` (default): NVENC when the host has an NVIDIA GPU and the render
   *   runs on it, libx264 otherwise.
   * - `"nvenc"`: h264_nvenc. If it fails, the render is redone once with
   *   libx264 and `RenderResult.encoderFallback` says why.
   * - `"x264"`: libx264.
   */
  encoder?: "auto" | "nvenc" | "x264";
  /**
   * x264 preset (default: `"veryfast"`). One of ultrafast, superfast, veryfast,
   * faster, fast, medium, slow, slower, veryslow, placebo.
   */
  preset?: string;
  /** NVENC preset, `"p1"` (fastest) to `"p7"` (slowest) (default: `"p4"`). */
  nvencPreset?: string;
  /**
   * Byte-reproducible output (default: false). Encodes mp4 with libx264 on one
   * thread and webm with libvpx on one thread, so the same frames always give
   * the same file regardless of CPU topology. Slower; used by tests that
   * compare files byte for byte. Cannot be combined with `encoder: "nvenc"`.
   */
  deterministic?: boolean;
}

/**
 * A row inside a `{ type: "list" }` PropertyValue. Each row instantiates a
 * ViewModelInstance and binds it into the parent VM's list property in the
 * order rows appear in the array.
 *
 * `viewModel` / `instance` mirror the top-level `ViewModelDataConfig` fields:
 *   - `viewModel` selects which VM type to instantiate the row from. If
 *     omitted, the renderer falls back to the artboard's default VM and then
 *     to the file's first VM; explicitly passing the item VM name is
 *     recommended for multi-VM files to avoid silent property mismatches.
 *   - `instance` selects a named instance preset to seed the row from.
 *     Default is the VM's default instance.
 *   - `properties` overrides individual properties on the row VM after
 *     instantiation. Recursively supports any `PropertyValue` (including
 *     nested `list` rows and `image` bindings).
 */
export interface ListItemConfig {
  viewModel?: string;
  instance?: string;
  properties?: Record<string, PropertyValue>;
}

export type PropertyValue =
  | { type: "string"; value: string }
  | { type: "number"; value: number }
  | { type: "boolean"; value: boolean }
  | { type: "color"; value: string }
  | { type: "enum"; value: string }
  /**
   * Bind a ViewModel image-property (sets `ViewModelInstanceAssetImage`).
   * `value` is an absolute filesystem path to a PNG/JPEG/WebP that the
   * native binary decodes and assigns. This is distinct from
   * `AssetConfig.images`, which substitutes file-referenced .riv assets
   * by name. Use `image` for VM-property bindings; use `assets.images`
   * for replacing referenced asset slots.
   */
  | { type: "image"; value: string }
  /**
   * Bind a ViewModel font-property (sets `ViewModelInstanceAssetFont`).
   * `value` is an absolute filesystem path to a TTF/OTF font file.
   */
  | { type: "font"; value: string }
  /**
   * Fire a ViewModel trigger-property once, after the view model is bound
   * and before the first frame advances.
   */
  | { type: "trigger" }
  /**
   * Bind a ViewModel data-list property (sets `ViewModelInstanceList`).
   * Each entry instantiates a ViewModelInstance and is appended to the
   * list in array order. Existing rows on the underlying instance are
   * cleared first so the rendered list matches `value` exactly.
   */
  | { type: "list"; value: ListItemConfig[] }
  /**
   * Bind an artboard from a second .riv file ("Rive on Rive") into an
   * artboard-typed ViewModel property (sets `ViewModelInstanceArtboard`).
   * The artboard is hosted by whatever nested-artboard slot the main file
   * binds to that property, and its first state machine runs there.
   *
   *   - `file` is a key of `RiveRenderConfig.extraFiles`.
   *   - `artboard` is the artboard to take from that file.
   *   - `viewModel` selects the view model of the extra file to instantiate
   *     for the artboard. If omitted, the artboard's own default view model
   *     is used, then the file's first one. The instance always comes from
   *     the extra file: the artboard would otherwise read the main file's
   *     view models and render empty.
   *   - `properties` sets properties on that instance. It accepts every
   *     `PropertyValue`, `image`, `font`, `list`, `trigger` and nested
   *     `artboard` bindings included.
   *
   * The render fails, naming the property, when the file key, the artboard
   * or the view model does not exist, or when the main view model has no
   * artboard property at this path.
   */
  | {
      type: "artboard";
      file: string;
      artboard: string;
      viewModel?: string;
      properties?: Record<string, PropertyValue>;
    };

export interface ViewModelDataConfig {
  /** ViewModel name (optional, uses default) */
  viewModel?: string;
  /** Instance name (optional) */
  instance?: string;
  /** Properties to set */
  properties: Record<string, PropertyValue>;
}

export interface AssetConfig {
  /** Map of asset name -> local file path for images */
  images?: Record<string, string>;
  /** Map of font name -> local file path for fonts */
  fonts?: Record<string, string>;
}

/**
 * A second .riv file whose artboards can be bound into artboard properties of
 * the main file's view model (see the `artboard` `PropertyValue`).
 */
export interface ExtraFileConfig {
  /** Path to the extra .riv file */
  rivFile: string;
  /**
   * Asset overrides for this file only. They are kept apart from the main
   * file's `assets`, so two files can use the same asset name with different
   * overrides.
   */
  assets?: AssetConfig;
}

export interface RiveRenderConfig {
  /** Path to .riv file */
  rivFile: string;
  /** Artboard name (optional, uses default) */
  artboard?: string;
  /** State machine name (optional, uses default) */
  stateMachine?: string;
  /**
   * Canvas width in pixels. If omitted, the artboard's own size is used;
   * if only one of width/height is given, the other follows the artboard's
   * aspect ratio. The resolved size is reported in `RenderResult`.
   */
  width?: number;
  /** Canvas height in pixels (see `width`). */
  height?: number;
  /** Screenshot config (mutually exclusive with output) */
  screenshot?: ScreenshotOptions;
  /** Animation output config (mutually exclusive with screenshot) */
  output?: OutputConfig;
  /** View model data to bind */
  viewModelData?: ViewModelDataConfig;
  /** Referenced assets to load */
  assets?: AssetConfig;
  /**
   * Extra .riv files, by alias, to take artboards from. Each is loaded with
   * its own `assets`; reference it from an `artboard` property value by its
   * key.
   */
  extraFiles?: Record<string, ExtraFileConfig>;
  /**
   * State machine input overrides. Numbers set number inputs, booleans set
   * boolean inputs; `true` on a trigger input fires it.
   */
  stateMachineInputs?: Record<string, boolean | number>;
  /**
   * Linux only: render with the bundled SwiftShader software Vulkan driver
   * instead of a GPU. Ignored on macOS, which always uses Metal.
   */
  swiftshader?: boolean;
  /** Path to ffmpeg binary (for GIF/MP4/WebM output). When omitted it is resolved from FFMPEG_PATH, then PATH, then a cached/downloaded copy. */
  ffmpegPath?: string;
}

export interface RenderOptions {
  /**
   * Aborting kills the native render process with SIGKILL and rejects with a
   * `RiveRenderError` (`exitCode` null). A signal that is already aborted
   * rejects without spawning anything.
   */
  signal?: AbortSignal;
}

export interface RenderResult {
  success: boolean;
  outputPath?: string;
  frameCount?: number;
  /** Canvas width actually rendered (useful when `width` was omitted). */
  width?: number;
  /** Canvas height actually rendered. */
  height?: number;
  /**
   * The ffmpeg codec that encoded the output, e.g. `"libx264"`, `"h264_nvenc"`,
   * `"libvpx-vp9"` or `"gif"`. Absent for png and screenshots.
   */
  encoder?: string;
  /** Why NVENC was not used when it failed and libx264 encoded instead. */
  encoderFallback?: string;
  error?: string;
}
