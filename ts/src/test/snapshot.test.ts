import { createHash } from "crypto";
import {
  copyFileSync,
  existsSync,
  mkdirSync,
  readFileSync,
  readdirSync,
  writeFileSync,
} from "fs";
import { resolve, join } from "path";
import { execSync } from "child_process";
import { toMatchImageSnapshot } from "jest-image-snapshot";
import { RiveRenderer, RiveRenderError } from "../index";

expect.extend({ toMatchImageSnapshot });

const FIXTURES = resolve(__dirname, "..", "..", "..", "test", "fixtures");
const BASKETBALL_RIV = resolve(FIXTURES, "basketball.riv"); // uses LinearAnimation
const STATEMACHINE_RIV = resolve(FIXTURES, "teststatemachine.riv"); // uses StateMachine
const TMP = "/tmp/rive-snapshot-work";

// Reference files saved in repo (images, gifs, mp4s)
// __dirname at runtime = dist/test/, resolve to src/test/__file_snapshots__
const FILE_SNAPSHOTS = resolve(__dirname, "..", "..", "src", "test", "__file_snapshots__");

const cli = new RiveRenderer();

beforeAll(() => {
  mkdirSync(TMP, { recursive: true });
  mkdirSync(FILE_SNAPSHOTS, { recursive: true });
});

// ─── Helpers ───

function sha256(buf: Buffer): string {
  return createHash("sha256").update(buf).digest("hex");
}

/**
 * Compare a rendered file against a committed reference.
 * First run: saves the file as reference.
 * Subsequent runs: asserts SHA-256 matches.
 * `jest -u`: overwrites the reference.
 */
function expectFileToMatchReference(actualPath: string, name: string) {
  const refPath = join(FILE_SNAPSHOTS, name);
  const actual = readFileSync(actualPath);
  const updating = process.argv.includes("-u") || process.argv.includes("--updateSnapshot");

  if (!existsSync(refPath) || updating) {
    copyFileSync(actualPath, refPath);
    return;
  }

  const reference = readFileSync(refPath);
  const actualHash = sha256(actual);
  const refHash = sha256(reference);

  if (actualHash !== refHash) {
    // Save the actual for inspection
    const diffPath = join(FILE_SNAPSHOTS, name.replace(/(\.\w+)$/, ".actual$1"));
    copyFileSync(actualPath, diffPath);

    throw new Error(
      `File snapshot "${name}" does not match reference.\n` +
        `  Expected SHA-256: ${refHash}\n` +
        `  Received SHA-256: ${actualHash}\n` +
        `  Reference: ${refPath}\n` +
        `  Actual: ${diffPath}\n` +
        `  Run with -u to update.`
    );
  }
}

async function renderToPng(options: {
  rivFile?: string;
  width?: number;
  height?: number;
  timestamp?: number;
  useCommandQueue?: boolean;
}): Promise<Buffer> {
  const tmp = `${TMP}/snap-${Date.now()}-${Math.random().toString(36).slice(2)}.png`;
  await cli.render({
    rivFile: options.rivFile ?? BASKETBALL_RIV,
    width: options.width ?? 400,
    height: options.height ?? 400,
    screenshot: { path: tmp, timestamp: options.timestamp ?? 0 },
    useCommandQueue: options.useCommandQueue,
  });
  const buf = readFileSync(tmp);
  require("fs").unlinkSync(tmp);
  return buf;
}

async function renderToFile(
  format: "gif" | "mp4",
  options: {
    rivFile?: string;
    width?: number;
    height?: number;
    fps?: number;
    duration: number;
    useCommandQueue?: boolean;
  }
): Promise<string> {
  const ext = format === "gif" ? ".gif" : ".mp4";
  const out = `${TMP}/snap-${Date.now()}-${Math.random().toString(36).slice(2)}${ext}`;
  await cli.render({
    rivFile: options.rivFile ?? BASKETBALL_RIV,
    width: options.width ?? 200,
    height: options.height ?? 200,
    output: {
      format,
      path: out,
      fps: options.fps ?? (format === "gif" ? 10 : 30),
      duration: options.duration,
    },
    useCommandQueue: options.useCommandQueue,
  });
  return out;
}

function extractFrames(
  videoPath: string,
  opts?: { maxFrames?: number }
): Buffer[] {
  const frameDir = `${TMP}/frames-${Date.now()}`;
  mkdirSync(frameDir, { recursive: true });

  const maxFrames = opts?.maxFrames ?? 999;
  execSync(
    `ffmpeg -y -i "${videoPath}" -frames:v ${maxFrames} "${frameDir}/frame-%03d.png" 2>/dev/null`
  );

  return readdirSync(frameDir)
    .filter((f) => f.endsWith(".png"))
    .sort()
    .map((f) => readFileSync(join(frameDir, f)));
}

// ─── Screenshots (basketball — LinearAnimation) ───

describe("Screenshots (LinearAnimation)", () => {
  const snapshotConfig = {
    failureThreshold: 0.001,
    failureThresholdType: "percent" as const,
  };

  it("t=0 (400x400)", async () => {
    const image = await renderToPng({ timestamp: 0 });
    expect(image).toMatchImageSnapshot({
      ...snapshotConfig,
      customSnapshotIdentifier: "basketball-t0-400x400",
    });
  });

  it("t=0.5s (400x400)", async () => {
    const image = await renderToPng({ timestamp: 0.5 });
    expect(image).toMatchImageSnapshot({
      ...snapshotConfig,
      customSnapshotIdentifier: "basketball-t0.5-400x400",
    });
  });

  it("t=1s (400x400)", async () => {
    const image = await renderToPng({ timestamp: 1.0 });
    expect(image).toMatchImageSnapshot({
      ...snapshotConfig,
      customSnapshotIdentifier: "basketball-t1-400x400",
    });
  });

  it("t=2s (400x400)", async () => {
    const image = await renderToPng({ timestamp: 2.0 });
    expect(image).toMatchImageSnapshot({
      ...snapshotConfig,
      customSnapshotIdentifier: "basketball-t2-400x400",
    });
  });

  it("t=1s small (100x100)", async () => {
    const image = await renderToPng({ width: 100, height: 100, timestamp: 1.0 });
    expect(image).toMatchImageSnapshot({
      ...snapshotConfig,
      customSnapshotIdentifier: "basketball-t1-100x100",
    });
  });

  it("t=1s large (800x800)", async () => {
    const image = await renderToPng({ width: 800, height: 800, timestamp: 1.0 });
    expect(image).toMatchImageSnapshot({
      ...snapshotConfig,
      customSnapshotIdentifier: "basketball-t1-800x800",
    });
  });

  it("t=1s via queue mode (400x400)", async () => {
    const image = await renderToPng({ timestamp: 1.0, useCommandQueue: true });
    expect(image).toMatchImageSnapshot({
      ...snapshotConfig,
      customSnapshotIdentifier: "basketball-t1-queue-400x400",
    });
  });

  it("same params produce identical images", async () => {
    const a = await renderToPng({ timestamp: 1.0, width: 200, height: 200 });
    const b = await renderToPng({ timestamp: 1.0, width: 200, height: 200 });
    expect(a.equals(b)).toBe(true);
  });

  it("different timestamps produce different images", async () => {
    const a = await renderToPng({ timestamp: 0, width: 200, height: 200 });
    const b = await renderToPng({ timestamp: 1.5, width: 200, height: 200 });
    expect(a.equals(b)).toBe(false);
  });
});

// ─── GIF (basketball — LinearAnimation) ───

describe("GIF (LinearAnimation)", () => {
  const frameSnapshotConfig = {
    failureThreshold: 0.5,
    failureThresholdType: "percent" as const,
  };

  it("1s 10fps — full file match", async () => {
    const gifPath = await renderToFile("gif", { fps: 10, duration: 1.0 });
    expectFileToMatchReference(gifPath, "basketball-1s-10fps.gif");
  });

  it("2s 10fps — full file match", async () => {
    const gifPath = await renderToFile("gif", { fps: 10, duration: 2.0 });
    expectFileToMatchReference(gifPath, "basketball-2s-10fps.gif");
  });

  it("1s 10fps via queue — full file match", async () => {
    const gifPath = await renderToFile("gif", { fps: 10, duration: 1.0, useCommandQueue: true });
    expectFileToMatchReference(gifPath, "basketball-1s-10fps-queue.gif");
  });

  it("1s 10fps — frame-by-frame visual regression", async () => {
    const gifPath = join(FILE_SNAPSHOTS, "basketball-1s-10fps.gif");
    const frames = extractFrames(gifPath);
    expect(frames.length).toBe(10);

    for (let i = 0; i < frames.length; i++) {
      expect(frames[i]).toMatchImageSnapshot({
        ...frameSnapshotConfig,
        customSnapshotIdentifier: `basketball-gif-10fps-frame-${String(i).padStart(2, "0")}`,
      });
    }
  });

  it("2s 10fps — key frame visual regression", async () => {
    const gifPath = join(FILE_SNAPSHOTS, "basketball-2s-10fps.gif");
    const frames = extractFrames(gifPath);
    expect(frames.length).toBe(20);

    for (const i of [0, 9, 19]) {
      expect(frames[i]).toMatchImageSnapshot({
        ...frameSnapshotConfig,
        customSnapshotIdentifier: `basketball-gif-2s-keyframe-${String(i).padStart(2, "0")}`,
      });
    }
  });

  it("1s 10fps via queue — frame-by-frame visual regression", async () => {
    const gifPath = join(FILE_SNAPSHOTS, "basketball-1s-10fps-queue.gif");
    const frames = extractFrames(gifPath);
    expect(frames.length).toBe(10);

    for (let i = 0; i < frames.length; i++) {
      expect(frames[i]).toMatchImageSnapshot({
        ...frameSnapshotConfig,
        customSnapshotIdentifier: `basketball-gif-queue-frame-${String(i).padStart(2, "0")}`,
      });
    }
  });

  it("respects fps * duration frame count", async () => {
    const gifPath = await renderToFile("gif", { fps: 15, duration: 2.0 });
    const frames = extractFrames(gifPath);
    expect(frames.length).toBe(30);
  });
});

// ─── MP4 (basketball — LinearAnimation) ───

describe("MP4 (LinearAnimation)", () => {
  const frameSnapshotConfig = {
    failureThreshold: 1.0,
    failureThresholdType: "percent" as const,
  };

  it("1s 30fps — full file match", async () => {
    const mp4Path = await renderToFile("mp4", { fps: 30, duration: 1.0 });
    expectFileToMatchReference(mp4Path, "basketball-1s-30fps.mp4");
  });

  it("2s 30fps — full file match", async () => {
    const mp4Path = await renderToFile("mp4", { fps: 30, duration: 2.0 });
    expectFileToMatchReference(mp4Path, "basketball-2s-30fps.mp4");
  });

  it("1s 30fps via queue — full file match", async () => {
    const mp4Path = await renderToFile("mp4", { fps: 30, duration: 1.0, useCommandQueue: true });
    expectFileToMatchReference(mp4Path, "basketball-1s-30fps-queue.mp4");
  });

  it("1s 30fps — key frame visual regression", async () => {
    const mp4Path = join(FILE_SNAPSHOTS, "basketball-1s-30fps.mp4");
    const frames = extractFrames(mp4Path);
    expect(frames.length).toBe(30);

    for (const i of [0, 7, 14, 22, 29]) {
      expect(frames[i]).toMatchImageSnapshot({
        ...frameSnapshotConfig,
        customSnapshotIdentifier: `basketball-mp4-30fps-keyframe-${String(i).padStart(2, "0")}`,
      });
    }
  });

  it("1s 30fps via queue — key frame visual regression", async () => {
    const mp4Path = join(FILE_SNAPSHOTS, "basketball-1s-30fps-queue.mp4");
    const frames = extractFrames(mp4Path);
    expect(frames.length).toBe(30);

    for (const i of [0, 14, 29]) {
      expect(frames[i]).toMatchImageSnapshot({
        ...frameSnapshotConfig,
        customSnapshotIdentifier: `basketball-mp4-queue-keyframe-${String(i).padStart(2, "0")}`,
      });
    }
  });

  it("frames show animation progression", async () => {
    const mp4Path = await renderToFile("mp4", { fps: 10, duration: 2.0 });
    const frames = extractFrames(mp4Path);
    expect(frames.length).toBe(20);

    const uniqueFrames = new Set(frames.map((f) => f.toString("base64")));
    expect(uniqueFrames.size).toBeGreaterThan(1);
  });
});

// ─── State Machine (teststatemachine.riv — StateMachine) ───

describe("StateMachine", () => {
  const snapshotConfig = {
    failureThreshold: 0.001,
    failureThresholdType: "percent" as const,
  };

  it("screenshot t=0 (200x200)", async () => {
    const image = await renderToPng({
      rivFile: STATEMACHINE_RIV,
      width: 200,
      height: 200,
      timestamp: 0,
    });
    expect(image).toMatchImageSnapshot({
      ...snapshotConfig,
      customSnapshotIdentifier: "statemachine-t0-200x200",
    });
  });

  it("screenshot t=0.5s (200x200)", async () => {
    const image = await renderToPng({
      rivFile: STATEMACHINE_RIV,
      width: 200,
      height: 200,
      timestamp: 0.5,
    });
    expect(image).toMatchImageSnapshot({
      ...snapshotConfig,
      customSnapshotIdentifier: "statemachine-t0.5-200x200",
    });
  });

  it("screenshot t=1s (200x200)", async () => {
    const image = await renderToPng({
      rivFile: STATEMACHINE_RIV,
      width: 200,
      height: 200,
      timestamp: 1.0,
    });
    expect(image).toMatchImageSnapshot({
      ...snapshotConfig,
      customSnapshotIdentifier: "statemachine-t1-200x200",
    });
  });

  it("animates — different timestamps produce different images", async () => {
    const a = await renderToPng({
      rivFile: STATEMACHINE_RIV,
      width: 200,
      height: 200,
      timestamp: 0,
    });
    const b = await renderToPng({
      rivFile: STATEMACHINE_RIV,
      width: 200,
      height: 200,
      timestamp: 0.5,
    });
    expect(a.equals(b)).toBe(false);
  });

  it("1s GIF — full file match", async () => {
    const gifPath = await renderToFile("gif", {
      rivFile: STATEMACHINE_RIV,
      fps: 10,
      duration: 1.0,
    });
    expectFileToMatchReference(gifPath, "statemachine-1s-10fps.gif");
  });

  it("1s GIF — frame-by-frame visual regression", async () => {
    const gifPath = join(FILE_SNAPSHOTS, "statemachine-1s-10fps.gif");
    const frames = extractFrames(gifPath);
    expect(frames.length).toBe(10);

    for (let i = 0; i < frames.length; i++) {
      expect(frames[i]).toMatchImageSnapshot({
        failureThreshold: 0.5,
        failureThresholdType: "percent",
        customSnapshotIdentifier: `statemachine-gif-10fps-frame-${String(i).padStart(2, "0")}`,
      });
    }
  });

  it("1s MP4 — full file match", async () => {
    const mp4Path = await renderToFile("mp4", {
      rivFile: STATEMACHINE_RIV,
      fps: 30,
      duration: 1.0,
    });
    expectFileToMatchReference(mp4Path, "statemachine-1s-30fps.mp4");
  });

  it("1s MP4 — key frame visual regression", async () => {
    const mp4Path = join(FILE_SNAPSHOTS, "statemachine-1s-30fps.mp4");
    const frames = extractFrames(mp4Path);
    expect(frames.length).toBe(30);

    for (const i of [0, 14, 29]) {
      expect(frames[i]).toMatchImageSnapshot({
        failureThreshold: 1.0,
        failureThresholdType: "percent",
        customSnapshotIdentifier: `statemachine-mp4-30fps-keyframe-${String(i).padStart(2, "0")}`,
      });
    }
  });
});

// ─── Error handling ───

describe("Error handling", () => {
  it("rejects for missing .riv file", async () => {
    await expect(
      cli.screenshot("/nonexistent/file.riv", { outputPath: `${TMP}/err.png` })
    ).rejects.toThrow(RiveRenderError);
  });

  it("rejects for invalid .riv data", async () => {
    const fakePath = `${TMP}/fake.riv`;
    writeFileSync(fakePath, "not a rive file");

    await expect(
      cli.screenshot(fakePath, { outputPath: `${TMP}/err.png` })
    ).rejects.toThrow(RiveRenderError);
  });

  it("rejects when no screenshot or output config provided", async () => {
    await expect(
      cli.render({ rivFile: BASKETBALL_RIV, width: 100, height: 100 })
    ).rejects.toThrow(RiveRenderError);
  });

  it("rejects for non-existent artboard name", async () => {
    await expect(
      cli.screenshot(BASKETBALL_RIV, {
        outputPath: `${TMP}/err.png`,
        artboard: "nonExistentArtboard_12345",
      })
    ).rejects.toThrow(RiveRenderError);
  });

  it("rejects for invalid binary path", async () => {
    const badCli = new RiveRenderer({ binaryPath: "/nonexistent/rive_render" });
    await expect(
      badCli.screenshot(BASKETBALL_RIV, { outputPath: `${TMP}/err.png` })
    ).rejects.toThrow(RiveRenderError);
  });
});
