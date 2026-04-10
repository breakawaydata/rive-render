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

function httpGet(url, extraHeaders) {
  return new Promise((resolve, reject) => {
    const follow = (currentUrl, hopCount) => {
      if (hopCount > 10) {
        reject(new Error(`Too many redirects for ${url}`));
        return;
      }
      const lib = currentUrl.startsWith("https") ? https : http;
      const parsed = new URL(currentUrl);
      const headers = {
        "User-Agent": "rive-render-postinstall",
        ...extraHeaders,
      };
      // Only send Authorization to GitHub hosts. When GitHub redirects to
      // S3 (or a CDN), forwarding the token causes signature-mismatch errors.
      if (
        process.env.GITHUB_TOKEN &&
        (parsed.hostname === "api.github.com" ||
          parsed.hostname === "github.com")
      ) {
        headers["Authorization"] = `token ${process.env.GITHUB_TOKEN}`;
      }
      lib
        .get(currentUrl, { headers }, (res) => {
          if (
            res.statusCode === 301 ||
            res.statusCode === 302 ||
            res.statusCode === 307 ||
            res.statusCode === 308
          ) {
            const location = res.headers.location;
            if (location) {
              // Resume any buffered data so the socket can close
              res.resume();
              follow(location, hopCount + 1);
              return;
            }
          }
          resolve(res);
        })
        .on("error", reject);
    };
    follow(url, 0);
  });
}

function downloadFile(url, dest) {
  return new Promise((resolve, reject) => {
    httpGet(url, { Accept: "application/octet-stream" })
      .then((res) => {
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
      .catch(reject);
  });
}

function fetchJson(url) {
  return new Promise((resolve, reject) => {
    httpGet(url, { Accept: "application/vnd.github+json" })
      .then((res) => {
        if (res.statusCode !== 200) {
          reject(new Error(`HTTP ${res.statusCode} from ${url}`));
          return;
        }
        let body = "";
        res.setEncoding("utf8");
        res.on("data", (chunk) => (body += chunk));
        res.on("end", () => {
          try {
            resolve(JSON.parse(body));
          } catch (err) {
            reject(err);
          }
        });
        res.on("error", reject);
      })
      .catch(reject);
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

  // Resolve asset URLs via the GitHub API. This works for both public and
  // private repos: for private repos, GITHUB_TOKEN is required, and we hit
  // the API URL with Accept: application/octet-stream to get the binary.
  const releaseUrl = `https://api.github.com/repos/breakawaydata/rive-render/releases/tags/v${version}`;
  let release;
  try {
    release = await fetchJson(releaseUrl);
  } catch (err) {
    console.error(
      `rive-render: Failed to fetch release metadata from ${releaseUrl}\n` +
        `  Error: ${err.message}\n` +
        `  You can set RIVE_RENDER_BINARY env var to a local binary path.`
    );
    return;
  }

  const binaryAsset = (release.assets || []).find((a) => a.name === assetName);
  if (!binaryAsset) {
    console.error(
      `rive-render: Release v${version} does not contain asset "${assetName}". ` +
        `You can set RIVE_RENDER_BINARY env var to a local binary path.`
    );
    return;
  }

  console.log(
    `rive-render: Downloading binary for ${platformKey} (v${version})...`
  );

  try {
    await downloadFile(binaryAsset.url, binaryPath);
    fs.chmodSync(binaryPath, 0o755);
    fs.writeFileSync(versionFile, version);
    console.log("rive-render: Binary installed successfully.");
  } catch (err) {
    // Clean up partial download
    try {
      fs.unlinkSync(binaryPath);
    } catch {
      // Ignore cleanup errors — the partial file may not exist
    }

    console.error(
      `rive-render: Failed to download binary from ${binaryAsset.url}\n` +
        `  Error: ${err.message}\n` +
        `  You can set RIVE_RENDER_BINARY env var to a local binary path.`
    );
    // Don't fail npm install — error will surface at runtime
    return;
  }

  // Download MoltenVK for macOS only if not already available on the system.
  // The binary's preloadMoltenVK() searches: bin dir, /opt/homebrew/lib, /usr/local/lib.
  // Run AFTER the binary try/catch so a MoltenVK failure can't delete the installed binary.
  if (os.platform() === "darwin") {
    const mvkPath = path.join(binDir, "libMoltenVK.dylib");
    const systemPaths = [
      mvkPath,
      "/opt/homebrew/lib/libMoltenVK.dylib",
      "/usr/local/lib/libMoltenVK.dylib",
    ];
    const found = systemPaths.some((p) => {
      try {
        return fs.existsSync(p);
      } catch {
        return false;
      }
    });
    if (!found) {
      const mvkAsset = (release.assets || []).find(
        (a) => a.name === "libMoltenVK.dylib"
      );
      if (mvkAsset) {
        try {
          await downloadFile(mvkAsset.url, mvkPath);
          console.log("rive-render: MoltenVK installed successfully.");
        } catch (mvkErr) {
          console.warn(
            `rive-render: Failed to download MoltenVK (${mvkErr.message}). ` +
              `Vulkan rendering may fail unless MoltenVK is installed via Homebrew.`
          );
        }
      }
    }
  }
}

main();
