import {
  accessSync,
  chmodSync,
  constants,
  createWriteStream,
  existsSync,
  mkdirSync,
  statSync,
  unlinkSync,
} from "fs";
import { delimiter, dirname, isAbsolute, join, resolve } from "path";
import { homedir, platform, arch } from "os";
import { execFileSync } from "child_process";
import https from "https";

// Nothing in here may need a shell: the hardened / distroless images this runs
// in have no /bin/sh, so `execSync`, `which` and shell-quoted commands all fail
// there. PATH is scanned directly and any helper process gets an argv array.

/** Env var that overrides where a downloaded ffmpeg is cached. */
export const CACHE_DIR_ENV = "RIVE_RENDER_CACHE_DIR";

const FFMPEG_BIN = platform() === "win32" ? "ffmpeg.exe" : "ffmpeg";

export interface ResolveFFmpegOptions {
  /** Explicit ffmpeg path; returned as-is when set. */
  ffmpegPath?: string;
  /** Environment to read FFMPEG_PATH, PATH and RIVE_RENDER_CACHE_DIR from (default: process.env). */
  env?: NodeJS.ProcessEnv;
}

function isExecutableFile(path: string): boolean {
  try {
    if (!statSync(path).isFile()) return false;
    accessSync(path, constants.X_OK);
    return true;
  } catch {
    return false;
  }
}

/**
 * The first `ffmpeg` on the given PATH string that is an executable regular
 * file, or undefined. Relative and empty entries are skipped rather than
 * resolved against the working directory.
 */
export function findOnPath(
  pathEnv: string | undefined,
  binary: string = FFMPEG_BIN
): string | undefined {
  for (const dir of (pathEnv ?? "").split(delimiter)) {
    if (!dir || !isAbsolute(dir)) continue;
    const candidate = join(dir, binary);
    if (isExecutableFile(candidate)) return candidate;
  }
  return undefined;
}

/** The ffmpeg cache directory: $RIVE_RENDER_CACHE_DIR, else ~/.rive-render. */
export function ffmpegCacheDir(env: NodeJS.ProcessEnv = process.env): string {
  const override = env[CACHE_DIR_ENV];
  if (override) return resolve(override);
  return join(homedir(), ".rive-render");
}

/**
 * Whether `dir` exists and is writable, or does not exist yet but its nearest
 * existing ancestor is writable (so it can be created).
 */
export function isWritableDir(dir: string): boolean {
  let current = resolve(dir);
  for (;;) {
    if (existsSync(current)) {
      try {
        if (!statSync(current).isDirectory()) return false;
        accessSync(current, constants.W_OK | constants.X_OK);
        return true;
      } catch {
        return false;
      }
    }
    const parent = dirname(current);
    if (parent === current) return false;
    current = parent;
  }
}

function downloadFile(url: string, dest: string): Promise<void> {
  return new Promise((resolve, reject) => {
    const follow = (url: string) => {
      https
        .get(url, (res) => {
          if (
            res.statusCode &&
            [301, 302, 303, 307, 308].includes(res.statusCode)
          ) {
            const location = res.headers.location;
            if (location) {
              res.resume();
              follow(new URL(location, url).toString());
              return;
            }
          }
          if (res.statusCode !== 200) {
            res.resume();
            reject(new Error(`Download failed: HTTP ${res.statusCode}`));
            return;
          }
          const file = createWriteStream(dest);
          file.on("error", reject);
          res.pipe(file);
          file.on("finish", () => {
            file.close();
            resolve();
          });
        })
        .on("error", reject);
    };
    follow(url);
  });
}

function downloadUrl(): string {
  const os = platform();
  const cpuArch = arch();
  if (os === "linux" && cpuArch === "x64") {
    return "https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-amd64-static.tar.xz";
  }
  if (os === "linux" && cpuArch === "arm64") {
    return "https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-arm64-static.tar.xz";
  }
  if (os === "darwin") {
    // For macOS, use evermeet.cx builds
    return "https://evermeet.cx/ffmpeg/getrelease/ffmpeg/zip";
  }
  throw new Error(
    `No auto-download available for ${os}-${cpuArch}. Please install ffmpeg manually.`
  );
}

/**
 * Find an ffmpeg binary. In order:
 *   1. `options.ffmpegPath`
 *   2. `$FFMPEG_PATH`
 *   3. the first executable `ffmpeg` in a `$PATH` directory (scanned directly, no `which`)
 *   4. a previously downloaded copy in the cache dir ($RIVE_RENDER_CACHE_DIR or ~/.rive-render)
 *   5. a static build downloaded into the cache dir — only if that dir is writable
 *
 * Throws an error naming every location tried when none of these work.
 */
export async function resolveFFmpeg(
  options: ResolveFFmpegOptions = {}
): Promise<string> {
  const env = options.env ?? process.env;

  if (options.ffmpegPath) return options.ffmpegPath;
  if (env.FFMPEG_PATH) return env.FFMPEG_PATH;

  const onPath = findOnPath(env.PATH);
  if (onPath) return onPath;

  const cacheDir = ffmpegCacheDir(env);
  const cachedPath = join(cacheDir, FFMPEG_BIN);
  if (isExecutableFile(cachedPath)) return cachedPath;

  const tried = [
    "FFMPEG_PATH (unset)",
    `PATH (${env.PATH ? env.PATH : "unset"})`,
    cachedPath,
  ].join(", ");

  if (!isWritableDir(cacheDir)) {
    throw new Error(
      `ffmpeg not found. Looked in: ${tried}. Not downloading a static build ` +
        `because the cache directory ${cacheDir} is not writable. Install ffmpeg on ` +
        `PATH, set FFMPEG_PATH, or point ${CACHE_DIR_ENV} at a writable directory.`
    );
  }

  const url = downloadUrl();
  console.error(`ffmpeg not found. Downloading static build into ${cacheDir}...`);
  mkdirSync(cacheDir, { recursive: true });

  const tmpPath = join(cacheDir, "ffmpeg-download");
  try {
    await downloadFile(url, tmpPath);
    // argv arrays, no shell. tar matches the '*/ffmpeg' wildcard itself.
    if (platform() === "darwin") {
      execFileSync("unzip", ["-o", tmpPath, "-d", cacheDir], { stdio: "pipe" });
    } else {
      execFileSync(
        "tar",
        ["-xf", tmpPath, "-C", cacheDir, "--strip-components=1", "--wildcards", "*/ffmpeg"],
        { stdio: "pipe" }
      );
    }
  } catch (err) {
    throw new Error(
      `ffmpeg not found (looked in: ${tried}) and downloading a static build into ` +
        `${cacheDir} failed: ${err instanceof Error ? err.message : String(err)}`,
      { cause: err }
    );
  } finally {
    try {
      unlinkSync(tmpPath);
    } catch {
      // Ignore cleanup errors
    }
  }

  if (existsSync(cachedPath)) {
    chmodSync(cachedPath, 0o755);
    console.error(`ffmpeg installed to: ${cachedPath}`);
    return cachedPath;
  }

  throw new Error(
    `ffmpeg not found (looked in: ${tried}) and the downloaded archive did not contain ffmpeg. ` +
      "Please install it manually."
  );
}
