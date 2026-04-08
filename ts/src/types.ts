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

export type PropertyValue =
  | { type: "string"; value: string }
  | { type: "number"; value: number }
  | { type: "boolean"; value: boolean }
  | { type: "color"; value: string }
  | { type: "enum"; value: string };

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
  /** Use CommandQueue/CommandServer mode (multi-threaded, matches app runtime pattern) */
  useCommandQueue?: boolean;
}

export interface RenderResult {
  success: boolean;
  outputPath?: string;
  frameCount?: number;
  error?: string;
}
