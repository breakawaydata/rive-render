import { spawn } from "child_process";
import { resolveBinary } from "./binary-resolver.js";
import { resolveFFmpeg } from "./ffmpeg-resolver.js";
import type {
  RiveRenderConfig,
  RenderResult,
  ViewModelDataConfig,
  AssetConfig,
} from "./types.js";

export class RiveRenderError extends Error {
  constructor(
    message: string,
    public readonly exitCode: number | null
  ) {
    super(message);
    this.name = "RiveRenderError";
  }
}

export class RiveRenderer {
  private binaryPath: string;

  constructor(options?: { binaryPath?: string }) {
    this.binaryPath = options?.binaryPath ?? resolveBinary();
  }

  async render(config: RiveRenderConfig): Promise<RenderResult> {
    // Auto-resolve ffmpeg for video formats
    if (
      config.output &&
      (config.output.format === "mp4" || config.output.format === "webm") &&
      !config.ffmpegPath
    ) {
      config = { ...config, ffmpegPath: await resolveFFmpeg() };
    }

    return new Promise((resolve, reject) => {
      const proc = spawn(this.binaryPath, [], {
        stdio: ["pipe", "pipe", "pipe"],
        env: {
          ...process.env,
          // Suppress MoltenVK info logging to keep stdout clean for JSON
          MVK_CONFIG_LOG_LEVEL: "1", // 0=none, 1=error, 2=warn, 3=info, 4=debug
        },
      });

      let stdout = "";
      let stderr = "";

      proc.stdout.on("data", (d: Buffer) => (stdout += d.toString()));
      proc.stderr.on("data", (d: Buffer) => (stderr += d.toString()));

      proc.on("close", (code) => {
        if (code !== 0) {
          reject(
            new RiveRenderError(
              stderr || `rive-render exited with code ${code}`,
              code
            )
          );
          return;
        }
        try {
          // The rive-runtime may print info lines to stdout (e.g. Vulkan GPU info).
          // Extract only the JSON line from the output.
          const jsonLine = stdout
            .split("\n")
            .find((line) => line.trimStart().startsWith("{"));
          if (!jsonLine) {
            reject(
              new RiveRenderError(`No JSON found in output: ${stdout}`, code)
            );
            return;
          }
          resolve(JSON.parse(jsonLine) as RenderResult);
        } catch {
          reject(
            new RiveRenderError(`Invalid JSON output: ${stdout}`, code)
          );
        }
      });

      proc.on("error", (err) => {
        reject(
          new RiveRenderError(
            `Failed to spawn rive-render: ${err.message}`,
            null
          )
        );
      });

      proc.stdin.write(JSON.stringify(config));
      proc.stdin.end();
    });
  }

  async screenshot(
    rivFile: string,
    options: {
      outputPath: string;
      width?: number;
      height?: number;
      timestamp?: number;
      artboard?: string;
      stateMachine?: string;
      viewModelData?: ViewModelDataConfig;
      assets?: AssetConfig;
    }
  ): Promise<RenderResult> {
    return this.render({
      rivFile,
      artboard: options.artboard,
      stateMachine: options.stateMachine,
      width: options.width ?? 800,
      height: options.height ?? 600,
      screenshot: {
        path: options.outputPath,
        timestamp: options.timestamp ?? 0,
      },
      viewModelData: options.viewModelData,
      assets: options.assets,
    });
  }

  async renderGif(
    rivFile: string,
    options: {
      outputPath: string;
      width?: number;
      height?: number;
      fps?: number;
      duration: number;
      artboard?: string;
      stateMachine?: string;
      viewModelData?: ViewModelDataConfig;
      assets?: AssetConfig;
    }
  ): Promise<RenderResult> {
    return this.render({
      rivFile,
      artboard: options.artboard,
      stateMachine: options.stateMachine,
      width: options.width ?? 800,
      height: options.height ?? 600,
      output: {
        format: "gif",
        path: options.outputPath,
        fps: options.fps ?? 30,
        duration: options.duration,
      },
      viewModelData: options.viewModelData,
      assets: options.assets,
    });
  }

  async renderVideo(
    rivFile: string,
    options: {
      outputPath: string;
      format?: "mp4" | "webm";
      width?: number;
      height?: number;
      fps?: number;
      duration: number;
      artboard?: string;
      stateMachine?: string;
      viewModelData?: ViewModelDataConfig;
      assets?: AssetConfig;
    }
  ): Promise<RenderResult> {
    return this.render({
      rivFile,
      artboard: options.artboard,
      stateMachine: options.stateMachine,
      width: options.width ?? 1920,
      height: options.height ?? 1080,
      output: {
        format: options.format ?? "mp4",
        path: options.outputPath,
        fps: options.fps ?? 60,
        duration: options.duration,
      },
      viewModelData: options.viewModelData,
      assets: options.assets,
    });
  }
}
