import { chmodSync, existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "fs";
import { tmpdir } from "os";
import path from "path";
import { RiveRenderer, RiveRenderError } from "../index.js";
import type { RiveRenderConfig } from "../index.js";

// These tests use a fake "native binary" (a shell script that records its pid
// and sleeps), so they need neither the built renderer nor ffmpeg.

const config: RiveRenderConfig = {
  rivFile: "unused.riv",
  width: 10,
  height: 10,
  screenshot: { path: "unused.png", timestamp: 0 },
};

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function waitFor(check: () => boolean, timeoutMs: number): Promise<boolean> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (check()) return true;
    await sleep(20);
  }
  return check();
}

function isAlive(pid: number): boolean {
  try {
    process.kill(pid, 0);
    return true;
  } catch (err) {
    if ((err as NodeJS.ErrnoException).code === "ESRCH") return false;
    throw err;
  }
}

describe("RiveRenderer.render abort signal", () => {
  let dir: string;
  let script: string;
  let pidFile: string;

  beforeEach(() => {
    dir = mkdtempSync(path.join(tmpdir(), "rive-render-abort-"));
    script = path.join(dir, "fake-render.sh");
    pidFile = path.join(dir, "pid");
    // Write the pid via rename so a reader never sees a half-written file.
    writeFileSync(
      script,
      `#!/bin/sh\necho $$ > "${pidFile}.tmp"\nmv "${pidFile}.tmp" "${pidFile}"\nexec sleep 600\n`
    );
    chmodSync(script, 0o755);
  });

  afterEach(() => {
    if (existsSync(pidFile)) {
      const pid = Number(readFileSync(pidFile, "utf8").trim());
      if (isAlive(pid)) process.kill(pid, "SIGKILL");
    }
    rmSync(dir, { recursive: true, force: true });
  });

  it("kills the native child and rejects when the signal aborts mid-render", async () => {
    const controller = new AbortController();
    const renderer = new RiveRenderer({ binaryPath: script });

    const result = renderer.render(config, { signal: controller.signal });
    const outcome = result.then(
      () => null,
      (err: unknown) => err
    );

    expect(await waitFor(() => existsSync(pidFile), 5000)).toBe(true);
    const pid = Number(readFileSync(pidFile, "utf8").trim());
    expect(isAlive(pid)).toBe(true);

    controller.abort();

    const err = await outcome;
    expect(err).toBeInstanceOf(RiveRenderError);
    expect((err as RiveRenderError).exitCode).toBeNull();
    expect((err as RiveRenderError).message).toContain("aborted");

    // The child may linger as a zombie until Node reaps it, so poll.
    expect(await waitFor(() => !isAlive(pid), 5000)).toBe(true);
  });

  it("includes the abort reason message when the reason is an Error", async () => {
    const controller = new AbortController();
    const renderer = new RiveRenderer({ binaryPath: script });

    const outcome = renderer.render(config, { signal: controller.signal }).then(
      () => null,
      (err: unknown) => err
    );
    expect(await waitFor(() => existsSync(pidFile), 5000)).toBe(true);

    controller.abort(new Error("job timed out"));

    const err = await outcome;
    expect(err).toBeInstanceOf(RiveRenderError);
    expect((err as RiveRenderError).message).toContain("job timed out");
  });

  it("rejects without spawning when the signal is already aborted", async () => {
    const controller = new AbortController();
    controller.abort();
    const renderer = new RiveRenderer({ binaryPath: script });

    await expect(
      renderer.render(config, { signal: controller.signal })
    ).rejects.toBeInstanceOf(RiveRenderError);

    // Give a wrongly spawned script time to write its pid file.
    await sleep(300);
    expect(existsSync(pidFile)).toBe(false);
  });
});
