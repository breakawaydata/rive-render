import { spawn } from "child_process";
import { resolveBinary } from "./binary-resolver.js";
import { resolveFFmpeg } from "./ffmpeg-resolver.js";
import type {
  RiveRenderConfig,
  RenderResult,
  RenderOptions,
  ViewModelDataConfig,
  AssetConfig,
  ExtraFileConfig,
  OutputConfig,
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

function abortedError(signal?: AbortSignal): RiveRenderError {
  const reason: unknown = signal?.reason;
  return new RiveRenderError(
    reason instanceof Error
      ? `rive-render aborted: ${reason.message}`
      : "rive-render aborted",
    null
  );
}

/**
 * Resolve with `work`, or reject with the aborted error as soon as `signal`
 * aborts. A failure of `work` after the abort also reports as aborted.
 */
function raceAbort<T>(work: Promise<T>, signal?: AbortSignal): Promise<T> {
  if (!signal) return work;
  return new Promise<T>((resolve, reject) => {
    const onAbort = () => reject(abortedError(signal));
    const cleanup = () => signal.removeEventListener("abort", onAbort);
    signal.addEventListener("abort", onAbort, { once: true });
    work.then(
      (value) => {
        cleanup();
        resolve(value);
      },
      (err: unknown) => {
        cleanup();
        reject(signal.aborted ? abortedError(signal) : err);
      }
    );
  });
}

export class RiveRenderer {
  private binaryPath: string;

  constructor(options?: { binaryPath?: string }) {
    this.binaryPath = options?.binaryPath ?? resolveBinary();
  }

  async render(
    config: RiveRenderConfig,
    options?: RenderOptions
  ): Promise<RenderResult> {
    const signal = options?.signal;
    if (signal?.aborted) throw abortedError(signal);

    // Auto-resolve ffmpeg for every format the native binary encodes with it
    if (
      config.output &&
      (config.output.format === "mp4" ||
        config.output.format === "webm" ||
        config.output.format === "gif") &&
      !config.ffmpegPath
    ) {
      // The resolver can download ffmpeg, so the signal may fire while waiting
      const ffmpegPath = await raceAbort(resolveFFmpeg(), signal);
      if (signal?.aborted) throw abortedError(signal);
      config = { ...config, ffmpegPath };
    }

    return new Promise((resolve, reject) => {
      const proc = spawn(this.binaryPath, [], {
        stdio: ["pipe", "pipe", "pipe"],
        // Own process group, so an abort can kill the ffmpeg the binary starts
        // along with the binary itself. No unref(): we still wait for close.
        detached: true,
        env: {
          ...process.env,
          // Suppress MoltenVK info logging to keep stdout clean for JSON
          MVK_CONFIG_LOG_LEVEL: "1", // 0=none, 1=error, 2=warn, 3=info, 4=debug
        },
      });

      let stdout = "";
      let stderr = "";
      let settled = false;
      let aborted = false;

      // Kill synchronously inside the abort event, but reject only once the
      // child has exited (close/error below), so a caller that retries on
      // rejection never overlaps the old render.
      const onAbort = () => {
        if (settled) return;
        aborted = true;
        try {
          // Negative pid signals the whole process group, ffmpeg included.
          process.kill(-(proc.pid as number), "SIGKILL");
        } catch {
          proc.kill("SIGKILL");
        }
      };
      const cleanup = () => signal?.removeEventListener("abort", onAbort);
      signal?.addEventListener("abort", onAbort, { once: true });

      // Writing the config to a killed child raises EPIPE on stdin; the
      // outcome is already reported through close/error/abort.
      proc.stdin.on("error", () => {});

      proc.stdout.on("data", (d: Buffer) => (stdout += d.toString()));
      proc.stderr.on("data", (d: Buffer) => (stderr += d.toString()));

      proc.on("close", (code) => {
        cleanup();
        if (settled) return;
        settled = true;
        if (aborted) {
          reject(abortedError(signal));
          return;
        }
        // The rive-runtime may print info lines to stdout (e.g. Vulkan GPU
        // info). The result is always the last line that is a JSON object.
        const jsonLine = stdout
          .split("\n")
          .reverse()
          .find((line) => line.trimStart().startsWith("{"));

        if (code !== 0) {
          // Prefer the binary's own error message; stderr may only hold
          // script / driver log noise.
          let message: string | undefined;
          try {
            message = jsonLine
              ? (JSON.parse(jsonLine) as RenderResult).error
              : undefined;
          } catch {
            // fall through to stderr
          }
          reject(
            new RiveRenderError(
              message || stderr || `rive-render exited with code ${code}`,
              code
            )
          );
          return;
        }
        try {
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
        cleanup();
        if (settled) return;
        settled = true;
        if (aborted) {
          reject(abortedError(signal));
          return;
        }
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
      extraFiles?: Record<string, ExtraFileConfig>;
    },
    renderOptions?: RenderOptions
  ): Promise<RenderResult> {
    return this.render(
      {
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
        extraFiles: options.extraFiles,
      },
      renderOptions
    );
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
      extraFiles?: Record<string, ExtraFileConfig>;
    },
    renderOptions?: RenderOptions
  ): Promise<RenderResult> {
    return this.render(
      {
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
        extraFiles: options.extraFiles,
      },
      renderOptions
    );
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
      /** See `OutputConfig.encoder`. */
      encoder?: OutputConfig["encoder"];
      /** See `OutputConfig.preset`. */
      preset?: string;
      /** See `OutputConfig.nvencPreset`. */
      nvencPreset?: string;
      /** See `OutputConfig.deterministic`. */
      deterministic?: boolean;
      artboard?: string;
      stateMachine?: string;
      viewModelData?: ViewModelDataConfig;
      assets?: AssetConfig;
      extraFiles?: Record<string, ExtraFileConfig>;
    },
    renderOptions?: RenderOptions
  ): Promise<RenderResult> {
    return this.render(
      {
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
          encoder: options.encoder,
          preset: options.preset,
          nvencPreset: options.nvencPreset,
          deterministic: options.deterministic,
        },
        viewModelData: options.viewModelData,
        assets: options.assets,
        extraFiles: options.extraFiles,
      },
      renderOptions
    );
  }
}
