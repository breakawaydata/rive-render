import { chmodSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from "fs";
import { tmpdir } from "os";
import { delimiter, join } from "path";

// Simulate an image with no /bin/sh: any attempt to run a shell command (the
// old `execSync("which ffmpeg")`) or any other child process fails the way it
// does in the hardened node image. The resolver must find ffmpeg without them.
jest.mock("child_process", () => {
  const noShell = () => {
    const err = new Error("spawnSync /bin/sh ENOENT") as NodeJS.ErrnoException;
    err.code = "ENOENT";
    throw err;
  };
  return {
    ...jest.requireActual("child_process"),
    exec: jest.fn(noShell),
    execSync: jest.fn(noShell),
    execFile: jest.fn(noShell),
    execFileSync: jest.fn(noShell),
    spawn: jest.fn(noShell),
    spawnSync: jest.fn(noShell),
  };
});

import * as childProcess from "child_process";
import {
  CACHE_DIR_ENV,
  ffmpegCacheDir,
  findOnPath,
  isWritableDir,
  resolveFFmpeg,
} from "../ffmpeg-resolver";

let work: string;

function makeFile(path: string, mode: number): string {
  mkdirSync(join(path, ".."), { recursive: true });
  // Content is irrelevant: the resolver only checks it is an executable file.
  writeFileSync(path, "not a real ffmpeg\n");
  chmodSync(path, mode);
  return path;
}

beforeEach(() => {
  work = mkdtempSync(join(tmpdir(), "rive-ffmpeg-resolver-"));
  jest.clearAllMocks();
});

afterEach(() => {
  // Restore write permission so cleanup can remove read-only fixtures.
  try {
    chmodSync(join(work, "readonly"), 0o755);
  } catch {
    // not created by this test
  }
  rmSync(work, { recursive: true, force: true });
});

function expectNoChildProcess() {
  for (const fn of ["exec", "execSync", "execFile", "execFileSync", "spawn", "spawnSync"]) {
    expect((childProcess as unknown as Record<string, jest.Mock>)[fn]).not.toHaveBeenCalled();
  }
}

describe("findOnPath", () => {
  it("returns the first executable ffmpeg, skipping missing dirs, non-executables and directories", () => {
    const missing = join(work, "missing");
    const nonExec = join(work, "nonexec");
    makeFile(join(nonExec, "ffmpeg"), 0o644);
    const dirNamedFfmpeg = join(work, "dirnamed");
    mkdirSync(join(dirNamedFfmpeg, "ffmpeg"), { recursive: true });
    const good = join(work, "good");
    const goodBin = makeFile(join(good, "ffmpeg"), 0o755);
    const later = join(work, "later");
    makeFile(join(later, "ffmpeg"), 0o755);

    const pathEnv = ["", "relative/bin", missing, nonExec, dirNamedFfmpeg, good, later].join(
      delimiter
    );
    expect(findOnPath(pathEnv)).toBe(goodBin);
  });

  it("returns undefined when PATH is unset or has no ffmpeg", () => {
    expect(findOnPath(undefined)).toBeUndefined();
    expect(findOnPath(join(work, "nothing-here"))).toBeUndefined();
  });
});

describe("resolveFFmpeg (no shell available)", () => {
  it("finds ffmpeg by scanning PATH directly", async () => {
    const bin = makeFile(join(work, "bin", "ffmpeg"), 0o755);
    const env = {
      PATH: [join(work, "empty"), join(work, "bin")].join(delimiter),
      [CACHE_DIR_ENV]: join(work, "cache"),
    };
    await expect(resolveFFmpeg({ env })).resolves.toBe(bin);
    expectNoChildProcess();
  });

  it("prefers an explicit path, then FFMPEG_PATH, over PATH", async () => {
    makeFile(join(work, "bin", "ffmpeg"), 0o755);
    const env = { PATH: join(work, "bin"), FFMPEG_PATH: "/opt/ffmpeg/bin/ffmpeg" };
    await expect(resolveFFmpeg({ env, ffmpegPath: "/explicit/ffmpeg" })).resolves.toBe(
      "/explicit/ffmpeg"
    );
    await expect(resolveFFmpeg({ env })).resolves.toBe("/opt/ffmpeg/bin/ffmpeg");
  });

  it("uses a cached ffmpeg from RIVE_RENDER_CACHE_DIR when PATH has none", async () => {
    const cacheDir = join(work, "cache");
    const cached = makeFile(join(cacheDir, "ffmpeg"), 0o755);
    const env = { PATH: join(work, "empty"), [CACHE_DIR_ENV]: cacheDir };
    expect(ffmpegCacheDir(env)).toBe(cacheDir);
    await expect(resolveFFmpeg({ env })).resolves.toBe(cached);
    expectNoChildProcess();
  });

  it("throws a clear error naming the paths tried when the cache dir is not writable", async () => {
    // Root ignores directory permissions, so this case cannot be staged as root.
    if (process.getuid && process.getuid() === 0) return;

    const readonly = join(work, "readonly");
    mkdirSync(readonly);
    chmodSync(readonly, 0o555);
    const cacheDir = join(readonly, ".rive-render");
    expect(isWritableDir(cacheDir)).toBe(false);

    const pathEnv = [join(work, "a"), join(work, "b")].join(delimiter);
    const env = { PATH: pathEnv, [CACHE_DIR_ENV]: cacheDir };
    const err = await resolveFFmpeg({ env }).catch((e: Error) => e);
    expect(err).toBeInstanceOf(Error);
    const message = (err as Error).message;
    expect(message).toContain("ffmpeg not found");
    expect(message).toContain(pathEnv);
    expect(message).toContain(join(cacheDir, "ffmpeg"));
    expect(message).toContain("not writable");
    expect(message).toContain(CACHE_DIR_ENV);
    expectNoChildProcess();
  });

  it("treats a missing cache dir under a writable parent as writable", () => {
    expect(isWritableDir(join(work, "not", "yet", "created"))).toBe(true);
  });
});
