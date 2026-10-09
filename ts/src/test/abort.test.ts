import { chmodSync, existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "fs";
import { tmpdir } from "os";
import path from "path";
import { RiveRenderer, RiveRenderError } from "../index.js";
import type { RiveRenderConfig } from "../index.js";
import { resolveFFmpeg } from "../ffmpeg-resolver.js";

// ffmpeg resolution can hit the network, so the resolver is replaced here. The
// other tests use a screenshot config, which never calls it.
jest.mock("../ffmpeg-resolver.js", () => ({ resolveFFmpeg: jest.fn() }));

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
  let grandchildPidFile: string;

  // Records pids via rename so a reader never sees a half-written file.
  function writeScript(file: string, body: string): void {
    writeFileSync(file, `#!/bin/sh\n${body}`);
    chmodSync(file, 0o755);
  }

  function readPid(file: string): number {
    return Number(readFileSync(file, "utf8").trim());
  }

  beforeEach(() => {
    dir = mkdtempSync(path.join(tmpdir(), "rive-render-abort-"));
    script = path.join(dir, "fake-render.sh");
    pidFile = path.join(dir, "pid");
    grandchildPidFile = path.join(dir, "grandchild-pid");
    writeScript(
      script,
      `echo $$ > "${pidFile}.tmp"\nmv "${pidFile}.tmp" "${pidFile}"\nexec sleep 600\n`
    );
    (resolveFFmpeg as jest.Mock).mockReset();
  });

  afterEach(() => {
    for (const file of [pidFile, grandchildPidFile]) {
      if (existsSync(file) && isAlive(readPid(file))) {
        process.kill(readPid(file), "SIGKILL");
      }
    }
    rmSync(dir, { recursive: true, force: true });
  });

  it("kills the native child and rejects only after it has exited", async () => {
    const controller = new AbortController();
    const renderer = new RiveRenderer({ binaryPath: script });

    const outcome = renderer.render(config, { signal: controller.signal }).then(
      () => null,
      (err: unknown) => err
    );

    expect(await waitFor(() => existsSync(pidFile), 5000)).toBe(true);
    const pid = readPid(pidFile);
    expect(isAlive(pid)).toBe(true);

    controller.abort();

    const err = await outcome;
    expect(err).toBeInstanceOf(RiveRenderError);
    expect((err as RiveRenderError).exitCode).toBeNull();
    expect((err as RiveRenderError).message).toContain("aborted");

    // The rejection waits for close, so the child is already reaped: no poll.
    expect(isAlive(pid)).toBe(false);
  });

  it("kills a grandchild (like ffmpeg) in the same process group", async () => {
    writeScript(
      script,
      [
        `sleep 600 &`,
        `echo $! > "${grandchildPidFile}.tmp"`,
        `mv "${grandchildPidFile}.tmp" "${grandchildPidFile}"`,
        `echo $$ > "${pidFile}.tmp"`,
        `mv "${pidFile}.tmp" "${pidFile}"`,
        `exec sleep 600`,
        ``,
      ].join("\n")
    );
    const controller = new AbortController();
    const renderer = new RiveRenderer({ binaryPath: script });

    const outcome = renderer.render(config, { signal: controller.signal }).then(
      () => null,
      (err: unknown) => err
    );

    expect(await waitFor(() => existsSync(pidFile), 5000)).toBe(true);
    const pid = readPid(pidFile);
    const grandchildPid = readPid(grandchildPidFile);
    expect(isAlive(pid)).toBe(true);
    expect(isAlive(grandchildPid)).toBe(true);

    controller.abort();

    expect(await outcome).toBeInstanceOf(RiveRenderError);
    expect(await waitFor(() => !isAlive(pid), 5000)).toBe(true);
    expect(await waitFor(() => !isAlive(grandchildPid), 5000)).toBe(true);
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

  it("rejects promptly when aborted while ffmpeg is still being resolved", async () => {
    (resolveFFmpeg as jest.Mock).mockReturnValue(new Promise(() => {}));
    const controller = new AbortController();
    const renderer = new RiveRenderer({ binaryPath: script });

    const outcome = renderer
      .render(
        {
          rivFile: "unused.riv",
          output: { format: "gif", path: "unused.gif", duration: 1 },
        },
        { signal: controller.signal }
      )
      .then(
        () => null,
        (err: unknown) => err
      );
    expect(resolveFFmpeg).toHaveBeenCalledTimes(1);

    controller.abort();

    const err = await outcome;
    expect(err).toBeInstanceOf(RiveRenderError);
    expect((err as RiveRenderError).message).toContain("aborted");

    await sleep(300);
    expect(existsSync(pidFile)).toBe(false);
  });
});
