import { existsSync, mkdirSync, createWriteStream, chmodSync } from "fs";
import { join } from "path";
import { homedir, platform, arch } from "os";
import { execSync } from "child_process";
import https from "https";

const CACHE_DIR = join(homedir(), ".rive-render");

function downloadFile(url: string, dest: string): Promise<void> {
  return new Promise((resolve, reject) => {
    const follow = (url: string) => {
      https
        .get(url, (res) => {
          if (res.statusCode === 301 || res.statusCode === 302) {
            const location = res.headers.location;
            if (location) {
              follow(location);
              return;
            }
          }
          if (res.statusCode !== 200) {
            reject(new Error(`Download failed: HTTP ${res.statusCode}`));
            return;
          }
          const file = createWriteStream(dest);
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

export async function resolveFFmpeg(): Promise<string> {
  // 1. Check FFMPEG_PATH env var
  if (process.env.FFMPEG_PATH) {
    return process.env.FFMPEG_PATH;
  }

  // 2. Check if ffmpeg is in system PATH
  try {
    const path = execSync("which ffmpeg", { encoding: "utf8" }).trim();
    if (path && existsSync(path)) {
      return path;
    }
  } catch {
    // Not in PATH
  }

  // 3. Check cache
  const cachedPath = join(CACHE_DIR, "ffmpeg");
  if (existsSync(cachedPath)) {
    return cachedPath;
  }

  // 4. Download static ffmpeg
  console.error("ffmpeg not found. Downloading static build...");
  mkdirSync(CACHE_DIR, { recursive: true });

  const os = platform();
  const cpuArch = arch();

  let url: string;
  if (os === "linux" && cpuArch === "x64") {
    url =
      "https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-amd64-static.tar.xz";
  } else if (os === "linux" && cpuArch === "arm64") {
    url =
      "https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-arm64-static.tar.xz";
  } else if (os === "darwin") {
    // For macOS, use evermeet.cx builds
    url = "https://evermeet.cx/ffmpeg/getrelease/ffmpeg/zip";
  } else {
    throw new Error(
      `No auto-download available for ${os}-${cpuArch}. Please install ffmpeg manually.`
    );
  }

  const tmpPath = join(CACHE_DIR, "ffmpeg-download");
  await downloadFile(url, tmpPath);

  if (os === "darwin") {
    // macOS zip
    execSync(`unzip -o "${tmpPath}" -d "${CACHE_DIR}"`, { stdio: "pipe" });
  } else {
    // Linux tar.xz
    execSync(
      `tar -xf "${tmpPath}" -C "${CACHE_DIR}" --strip-components=1 --wildcards '*/ffmpeg'`,
      { stdio: "pipe" }
    );
  }

  // Clean up download
  try {
    const fs = await import("fs");
    fs.unlinkSync(tmpPath);
  } catch {
    // Ignore cleanup errors
  }

  if (existsSync(cachedPath)) {
    chmodSync(cachedPath, 0o755);
    console.error(`ffmpeg installed to: ${cachedPath}`);
    return cachedPath;
  }

  throw new Error("Failed to download ffmpeg. Please install it manually.");
}
