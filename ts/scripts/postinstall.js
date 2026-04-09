#!/usr/bin/env node
"use strict";

const https = require("https");
const http = require("http");
const fs = require("fs");
const path = require("path");
const os = require("os");

const SUPPORTED_PLATFORMS = {
  "darwin-arm64": "rive-render-darwin-arm64",
  "darwin-x64": "rive-render-darwin-x64",
  "linux-x64": "rive-render-linux-x64",
  "linux-arm64": "rive-render-linux-arm64",
};

function getPlatformKey() {
  return `${os.platform()}-${os.arch()}`;
}

function getVersion() {
  const pkg = JSON.parse(
    fs.readFileSync(path.join(__dirname, "..", "package.json"), "utf8")
  );
  return pkg.version;
}

function downloadFile(url, dest) {
  return new Promise((resolve, reject) => {
    const follow = (url) => {
      const lib = url.startsWith("https") ? https : http;
      const headers = { "User-Agent": "rive-render-postinstall" };
      if (process.env.GITHUB_TOKEN) {
        headers["Authorization"] = `token ${process.env.GITHUB_TOKEN}`;
      }
      lib
        .get(url, { headers }, (res) => {
          if (res.statusCode === 301 || res.statusCode === 302) {
            const location = res.headers.location;
            if (location) {
              follow(location);
              return;
            }
          }
          if (res.statusCode !== 200) {
            reject(
              new Error(`Download failed: HTTP ${res.statusCode} from ${url}`)
            );
            return;
          }
          const file = fs.createWriteStream(dest);
          res.pipe(file);
          file.on("finish", () => {
            file.close();
            resolve();
          });
          file.on("error", reject);
        })
        .on("error", reject);
    };
    follow(url);
  });
}

async function main() {
  // Skip if user is providing their own binary
  if (process.env.RIVE_RENDER_BINARY) {
    console.log(
      "rive-render: RIVE_RENDER_BINARY is set, skipping binary download."
    );
    return;
  }

  // Skip during CI publishing or development
  if (process.env.RIVE_RENDER_SKIP_POSTINSTALL === "1") {
    return;
  }

  const platformKey = getPlatformKey();
  const assetName = SUPPORTED_PLATFORMS[platformKey];

  if (!assetName) {
    console.warn(
      `rive-render: Unsupported platform ${platformKey}. ` +
        `Supported: ${Object.keys(SUPPORTED_PLATFORMS).join(", ")}. ` +
        `Set RIVE_RENDER_BINARY env var to provide a custom binary path.`
    );
    return;
  }

  const version = getVersion();
  const url = `https://github.com/breakawaydata/rive-render/releases/download/v${version}/${assetName}`;

  const binDir = path.join(__dirname, "..", "bin");
  const binaryPath = path.join(binDir, "rive-render");
  const versionFile = path.join(binDir, ".version");

  // Skip if binary already exists for this version
  if (fs.existsSync(binaryPath) && fs.existsSync(versionFile)) {
    const installedVersion = fs.readFileSync(versionFile, "utf8").trim();
    if (installedVersion === version) {
      return;
    }
  }

  fs.mkdirSync(binDir, { recursive: true });

  console.log(
    `rive-render: Downloading binary for ${platformKey} (v${version})...`
  );

  try {
    await downloadFile(url, binaryPath);
    fs.chmodSync(binaryPath, 0o755);
    fs.writeFileSync(versionFile, version);
    console.log("rive-render: Binary installed successfully.");

    // Download MoltenVK for macOS (Vulkan-on-Metal, needed by the binary)
    if (os.platform() === "darwin") {
      const mvkUrl = `https://github.com/breakawaydata/rive-render/releases/download/v${version}/libMoltenVK.dylib`;
      const mvkPath = path.join(binDir, "libMoltenVK.dylib");
      try {
        await downloadFile(mvkUrl, mvkPath);
        console.log("rive-render: MoltenVK installed successfully.");
      } catch (mvkErr) {
        console.warn(
          `rive-render: Failed to download MoltenVK (${mvkErr.message}). ` +
            `Vulkan rendering may fail unless MoltenVK is installed via Homebrew.`
        );
      }
    }
  } catch (err) {
    // Clean up partial download
    try {
      fs.unlinkSync(binaryPath);
    } catch {}

    console.error(
      `rive-render: Failed to download binary from ${url}\n` +
        `  Error: ${err.message}\n` +
        `  You can set RIVE_RENDER_BINARY env var to a local binary path.`
    );
    // Don't fail npm install — error will surface at runtime
  }
}

main();
