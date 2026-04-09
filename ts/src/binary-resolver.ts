import { existsSync } from "fs";
import { join } from "path";
import { platform, arch } from "os";

const SUPPORTED_PLATFORMS = [
  "darwin-arm64",
  "darwin-x64",
  "linux-x64",
  "linux-arm64",
];

export function resolveBinary(): string {
  // 1. Check RIVE_RENDER_BINARY env var
  if (process.env.RIVE_RENDER_BINARY) {
    return process.env.RIVE_RENDER_BINARY;
  }

  const platformKey = `${platform()}-${arch()}`;

  // 2. Check for postinstall-downloaded binary (bin/ within this package)
  const postinstallBinary = join(__dirname, "..", "bin", "rive-render");
  if (existsSync(postinstallBinary)) {
    return postinstallBinary;
  }

  // 3. Check local development build
  const localBuild = join(
    __dirname,
    "..",
    "..",
    "native",
    "out",
    "release",
    "rive_render"
  );
  if (existsSync(localBuild)) {
    return localBuild;
  }

  const supportedMsg = SUPPORTED_PLATFORMS.includes(platformKey)
    ? "The postinstall binary download may have failed. Try reinstalling the package."
    : `Platform ${platformKey} is not supported (supported: ${SUPPORTED_PLATFORMS.join(", ")}).`;

  throw new Error(
    `No rive-render binary found for ${platformKey}. ${supportedMsg} ` +
      `You can set RIVE_RENDER_BINARY env var to provide a custom binary path.`
  );
}
