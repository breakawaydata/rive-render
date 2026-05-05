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
}

/**
 * A row inside a `{ type: "list" }` PropertyValue. Each row instantiates a
 * ViewModelInstance and binds it into the parent VM's list property in the
 * order rows appear in the array.
 *
 * `viewModel` / `instance` mirror the top-level `ViewModelDataConfig` fields:
 *   - `viewModel` selects which VM type to instantiate the row from. Default
 *     is the list element's declared item-VM type.
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
   * Bind a ViewModel data-list property (sets `ViewModelInstanceList`).
   * Each entry instantiates a ViewModelInstance and is appended to the
   * list in array order. Existing rows on the underlying instance are
   * cleared first so the rendered list matches `value` exactly.
   */
  | { type: "list"; value: ListItemConfig[] };

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

export interface RiveRenderConfig {
  /** Path to .riv file */
  rivFile: string;
  /** Artboard name (optional, uses default) */
  artboard?: string;
  /** State machine name (optional, uses default) */
  stateMachine?: string;
  /** Canvas width in pixels */
  width: number;
  /** Canvas height in pixels */
  height: number;
  /** Screenshot config (mutually exclusive with output) */
  screenshot?: ScreenshotOptions;
  /** Animation output config (mutually exclusive with screenshot) */
  output?: OutputConfig;
  /** View model data to bind */
  viewModelData?: ViewModelDataConfig;
  /** Referenced assets to load */
  assets?: AssetConfig;
  /** State machine input overrides */
  stateMachineInputs?: Record<string, boolean | number>;
  /** Path to ffmpeg binary (for video output) */
  ffmpegPath?: string;
}

export interface RenderResult {
  success: boolean;
  outputPath?: string;
  frameCount?: number;
  error?: string;
}
