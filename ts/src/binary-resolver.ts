import { existsSync } from "fs";
import { join } from "path";
import { platform, arch } from "os";

export function resolveBinary(): string {
  // 1. Check RIVE_RENDER_BINARY env var
  if (process.env.RIVE_RENDER_BINARY) {
    return process.env.RIVE_RENDER_BINARY;
  }

  // 2. Check platform-specific optional dependency package
  const platformKey = `${platform()}-${arch()}`;
  const packageMap: Record<string, string> = {
    "darwin-arm64": "@breakaway/rive-render-darwin-arm64",
    "darwin-x64": "@breakaway/rive-render-darwin-x64",
    "linux-x64": "@breakaway/rive-render-linux-x64",
    "linux-arm64": "@breakaway/rive-render-linux-arm64",
  };

  const pkg = packageMap[platformKey];
  if (pkg) {
    try {
      return require.resolve(`${pkg}/bin/rive-render`);
    } catch {
      // Not installed, fall through
    }
  }

  // 3. Check local build (relative to this package)
  const localPaths = [
    join(__dirname, "..", "..", "native", "out", "release", "rive_render"),
    join(__dirname, "..", "bin", "rive-render"),
  ];
  for (const p of localPaths) {
    if (existsSync(p)) return p;
  }

  throw new Error(
    `No rive-render binary found for ${platformKey}. ` +
      `Set RIVE_RENDER_BINARY env var or install @breakaway/rive-render-${platformKey}.`
  );
}
