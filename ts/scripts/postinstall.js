#!/usr/bin/env node
"use strict";

const https = require("https");
const fs = require("fs");
const path = require("path");
const os = require("os");

const REPO = "breakawaydata/rive-render";
const SUPPORTED_PLATFORMS = {
  "darwin-arm64": "rive-render-darwin-arm64",
  "darwin-x64": "rive-render-darwin-x64",
  "linux-x64": "rive-render-linux-x64",
  "linux-arm64": "rive-render-linux-arm64",
};

function download(url, dest, redirectsLeft = 10) {
  return new Promise((resolve, reject) => {
    https
      .get(url, { headers: { "User-Agent": "rive-render-postinstall" } }, (res) => {
        if (res.statusCode >= 300 && res.statusCode < 400 && res.headers.location) {
          res.resume();
          if (redirectsLeft <= 0) {
            reject(new Error(`Too many redirects for ${url}`));
            return;
          }
          download(res.headers.location, dest, redirectsLeft - 1).then(resolve, reject);
          return;
        }
        if (res.statusCode !== 200) {
          reject(new Error(`HTTP ${res.statusCode} from ${url}`));
          return;
        }
        const file = fs.createWriteStream(dest);
        res.pipe(file);
        file.on("finish", () => file.close(resolve));
        file.on("error", reject);
      })
      .on("error", reject);
  });
}

async function main() {
  if (process.env.RIVE_RENDER_BINARY) return;
  if (process.env.RIVE_RENDER_SKIP_POSTINSTALL === "1") return;

  const platformKey = `${os.platform()}-${os.arch()}`;
  const assetName = SUPPORTED_PLATFORMS[platformKey];
  if (!assetName) {
    console.warn(
      `rive-render: Unsupported platform ${platformKey}. ` +
        `Supported: ${Object.keys(SUPPORTED_PLATFORMS).join(", ")}. ` +
        `Set RIVE_RENDER_BINARY env var to provide a custom binary path.`
    );
    return;
  }

  const { version } = JSON.parse(
    fs.readFileSync(path.join(__dirname, "..", "package.json"), "utf8")
  );
  const binDir = path.join(__dirname, "..", "bin");
  const binaryPath = path.join(binDir, "rive-render");
  const versionFile = path.join(binDir, ".version");

  // Skip if binary is already installed for this version
  if (fs.existsSync(versionFile) && fs.readFileSync(versionFile, "utf8").trim() === version) {
    return;
  }

  fs.mkdirSync(binDir, { recursive: true });
  const releaseUrl = `https://github.com/${REPO}/releases/download/v${version}`;

  console.log(`rive-render: Downloading binary for ${platformKey} (v${version})...`);
  try {
    await download(`${releaseUrl}/${assetName}`, binaryPath);
    fs.chmodSync(binaryPath, 0o755);
    fs.writeFileSync(versionFile, version);
  } catch (err) {
    try { fs.unlinkSync(binaryPath); } catch { /* ignore */ }
    console.error(
      `rive-render: Failed to download binary: ${err.message}\n` +
        `  You can set RIVE_RENDER_BINARY env var to a local binary path.`
    );
    return;
  }

  // On Linux, also fetch the bundled SwiftShader ICD next to the binary.
  // The rive-render binary enables it via `"swiftshader": true` in the
  // JSON config, which points VK_ICD_FILENAMES at the sibling json file.
  // Users on real-GPU machines can ignore these files and use the system
  // Vulkan driver. On macOS we render through Metal — no Vulkan fallback.
  if (os.platform() === "linux") {
    const arch = os.arch() === "arm64" ? "arm64" : "x64";
    const ssAssets = [
      {
        remote: `libvk_swiftshader-linux-${arch}.so`,
        local: "libvk_swiftshader.so",
      },
      {
        remote: `vk_swiftshader_icd-linux-${arch}.json`,
        local: "vk_swiftshader_icd.json",
      },
    ];
    for (const asset of ssAssets) {
      try {
        await download(`${releaseUrl}/${asset.remote}`, path.join(binDir, asset.local));
      } catch (err) {
        console.warn(
          `rive-render: Failed to download SwiftShader asset ${asset.remote} ` +
            `(${err.message}). The "swiftshader": true config flag may not work until ` +
            `these files are placed next to the rive-render binary.`
        );
        break;
      }
    }
  }
}

main();
