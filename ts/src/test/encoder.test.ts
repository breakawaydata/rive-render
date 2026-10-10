import { execFileSync, spawnSync } from "child_process";
import { chmodSync, existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "fs";
import { tmpdir } from "os";
import { basename, dirname, join, resolve } from "path";
import { RiveRenderer, RiveRenderError } from "../index.js";
import type { OutputConfig, RenderResult } from "../index.js";
import { resolveBinary } from "../binary-resolver.js";
import { resolveFFmpeg } from "../ffmpeg-resolver.js";

// Video encoder selection, the ffmpeg arguments it produces, the NVENC -> x264 fallback, and the
// GPU/ICD auto-selection. None of it needs a GPU: a fake ffmpeg stands in for NVENC, and
// `rive_render --select-device` reports the decisions without rendering.

const BASKETBALL_RIV = resolve(__dirname, "..", "..", "..", "test", "fixtures", "basketball.riv");
const IS_MAC = process.platform === "darwin";

// The input half of every ffmpeg command line rive-render builds.
function inputArgs(width: number, height: number, fps: number): string[] {
  return ["-y", "-f", "rawvideo", "-pix_fmt", "rgba", "-s", `${width}x${height}`, "-r", `${fps}`, "-i", "pipe:0"];
}

const X264_DEFAULT = ["-c:v", "libx264", "-pix_fmt", "yuv420p", "-preset", "veryfast", "-crf", "23"];
// The command line before encoder options were configurable (preset medium, one thread).
const X264_PRE_OPTIONS = [
  "-c:v", "libx264", "-pix_fmt", "yuv420p", "-preset", "medium", "-crf", "23",
  "-x264-params", "threads=1:sliced-threads=0",
];
const NVENC_DEFAULT = [
  "-c:v", "h264_nvenc", "-pix_fmt", "yuv420p", "-preset", "p4",
  "-rc", "vbr", "-cq", "23", "-b:v", "0", "-profile:v", "high",
];

const FAKE_FFMPEG = `#!${process.execPath}
const fs = require("fs");
const { spawn } = require("child_process");
const argv = process.argv.slice(2);
fs.appendFileSync(process.env.FAKE_FFMPEG_LOG, JSON.stringify(argv) + "\\n");
if (process.env.FAKE_FFMPEG_PID_FILE) fs.writeFileSync(process.env.FAKE_FFMPEG_PID_FILE, String(process.pid));
const mode = process.env.FAKE_FFMPEG_MODE;
if (mode === "fail-all" || (mode === "fail-nvenc" && argv.includes("h264_nvenc"))) {
  process.stderr.write("fake ffmpeg: refusing to encode with " + argv[argv.indexOf("-c:v") + 1] + "\\n");
  process.exit(1);
}
if (mode === "drain") {
  process.stdin.resume();
  process.stdin.on("end", () => process.exit(0));
} else {
  const child = spawn(process.env.REAL_FFMPEG, argv, { stdio: "inherit" });
  child.on("exit", (code) => process.exit(code === null ? 1 : code));
}
`;

type FakeMode = "drain" | "fail-nvenc" | "fail-all";

let work: string;
let realFfmpeg: string;
let fakeFfmpeg: string;
let logFile: string;
let pidFile: string;
const cli = new RiveRenderer();
const touchedEnv = [
  "FAKE_FFMPEG_LOG",
  "FAKE_FFMPEG_MODE",
  "FAKE_FFMPEG_PID_FILE",
  "REAL_FFMPEG",
  "RIVE_RENDER_NVIDIACTL_PATH",
  "VK_ICD_FILENAMES",
  "VK_DRIVER_FILES",
];
const savedEnv: Record<string, string | undefined> = {};

beforeAll(async () => {
  work = mkdtempSync(join(tmpdir(), "rive-render-encoder-"));
  realFfmpeg = await resolveFFmpeg();
  fakeFfmpeg = join(work, "fake-ffmpeg");
  writeFileSync(fakeFfmpeg, FAKE_FFMPEG);
  chmodSync(fakeFfmpeg, 0o755);
  logFile = join(work, "ffmpeg-argv.log");
  pidFile = join(work, "ffmpeg.pid");
  for (const name of touchedEnv) savedEnv[name] = process.env[name];
});

afterAll(() => {
  for (const name of touchedEnv) {
    if (savedEnv[name] === undefined) delete process.env[name];
    else process.env[name] = savedEnv[name];
  }
  rmSync(work, { recursive: true, force: true });
});

beforeEach(() => {
  writeFileSync(logFile, "");
  rmSync(pidFile, { force: true });
  process.env.FAKE_FFMPEG_LOG = logFile;
  process.env.FAKE_FFMPEG_PID_FILE = pidFile;
  process.env.REAL_FFMPEG = realFfmpeg;
  // "auto" must not depend on the machine running the tests: no NVIDIA device node, no caller ICD.
  process.env.RIVE_RENDER_NVIDIACTL_PATH = join(work, "no-such-nvidiactl");
  delete process.env.VK_ICD_FILENAMES;
  delete process.env.VK_DRIVER_FILES;
});

function isAlive(pid: number): boolean {
  try {
    process.kill(pid, 0);
    return true;
  } catch (err) {
    if ((err as NodeJS.ErrnoException).code === "ESRCH") return false;
    throw err;
  }
}

/** Every ffmpeg command line the render started, in order. */
function ffmpegInvocations(): string[][] {
  return readFileSync(logFile, "utf8")
    .split("\n")
    .filter((line) => line.length > 0)
    .map((line) => JSON.parse(line) as string[]);
}

function renderMp4(
  output: Partial<OutputConfig>,
  mode: FakeMode | "forward",
  size = 100,
  extra: { ffmpegPath?: string; artboard?: string } = {}
): { path: string; result: Promise<RenderResult> } {
  process.env.FAKE_FFMPEG_MODE = mode;
  const path = join(work, `out-${Date.now()}-${Math.random().toString(36).slice(2)}.${output.format ?? "mp4"}`);
  const result = cli.render({
    rivFile: BASKETBALL_RIV,
    width: size,
    height: size,
    ffmpegPath: extra.ffmpegPath ?? fakeFfmpeg,
    artboard: extra.artboard,
    output: { format: "mp4", fps: 30, duration: 0.2, ...output, path },
  });
  return { path, result };
}

function ffprobeVideo(file: string): {
  codec_name: string;
  pix_fmt: string;
  width: number;
  height: number;
  nb_read_frames: string;
} {
  const sibling = join(dirname(realFfmpeg), basename(realFfmpeg).replace("ffmpeg", "ffprobe"));
  const ffprobe = realFfmpeg.includes("/") && existsSync(sibling) ? sibling : "ffprobe";
  const out = execFileSync(ffprobe, [
    "-v", "error", "-count_frames", "-select_streams", "v:0",
    "-show_entries", "stream=codec_name,pix_fmt,width,height,nb_read_frames",
    "-of", "json", file,
  ]).toString();
  return (JSON.parse(out) as { streams: ReturnType<typeof ffprobeVideo>[] }).streams[0];
}

describe("ffmpeg arguments", () => {
  it("defaults to x264 veryfast with automatic threads", async () => {
    const { path, result } = renderMp4({}, "drain");
    const res = await result;

    expect(ffmpegInvocations()).toEqual([[...inputArgs(100, 100, 30), ...X264_DEFAULT, path]]);
    expect(res.encoder).toBe("libx264");
    expect(res.encoderFallback).toBeUndefined();
    expect(res.frameCount).toBe(6);
  });

  it("deterministic with preset medium is exactly the command line from before the options", async () => {
    const { path, result } = renderMp4({ deterministic: true, preset: "medium" }, "drain");
    await result;

    expect(ffmpegInvocations()).toEqual([[...inputArgs(100, 100, 30), ...X264_PRE_OPTIONS, path]]);
  });

  it("deterministic alone keeps one thread and the default preset", async () => {
    const { path, result } = renderMp4({ deterministic: true }, "drain");
    await result;

    const expected = [
      "-c:v", "libx264", "-pix_fmt", "yuv420p", "-preset", "veryfast", "-crf", "23",
      "-x264-params", "threads=1:sliced-threads=0",
    ];
    expect(ffmpegInvocations()).toEqual([[...inputArgs(100, 100, 30), ...expected, path]]);
  });

  it("passes a chosen x264 preset", async () => {
    const { path, result } = renderMp4({ preset: "slow" }, "drain");
    await result;

    const expected = [...X264_DEFAULT];
    expected[expected.indexOf("veryfast")] = "slow";
    expect(ffmpegInvocations()).toEqual([[...inputArgs(100, 100, 30), ...expected, path]]);
  });

  it("encoder nvenc uses h264_nvenc with the NVENC preset and constant quality", async () => {
    const { path, result } = renderMp4({ encoder: "nvenc" }, "drain");
    const res = await result;

    expect(ffmpegInvocations()).toEqual([[...inputArgs(100, 100, 30), ...NVENC_DEFAULT, path]]);
    expect(res.encoder).toBe("h264_nvenc");
    expect(res.encoderFallback).toBeUndefined();
  });

  it("passes a chosen NVENC preset", async () => {
    const { path, result } = renderMp4({ encoder: "nvenc", nvencPreset: "p7" }, "drain");
    await result;

    const expected = [...NVENC_DEFAULT];
    expected[expected.indexOf("p4")] = "p7";
    expect(ffmpegInvocations()).toEqual([[...inputArgs(100, 100, 30), ...expected, path]]);
  });

  it("auto picks NVENC when an NVIDIA device node exists and the render is not on SwiftShader", async () => {
    process.env.RIVE_RENDER_NVIDIACTL_PATH = join(work, "fake-nvidiactl");
    writeFileSync(process.env.RIVE_RENDER_NVIDIACTL_PATH, "");
    const { result } = renderMp4({}, "drain");
    const res = await result;

    // macOS renders with Metal and always encodes with x264.
    expect(res.encoder).toBe(IS_MAC ? "libx264" : "h264_nvenc");
  });

  it("webm: automatic threads by default, one thread when deterministic", async () => {
    const base = [
      "-c:v", "libvpx-vp9", "-pix_fmt", "yuv420p", "-crf", "30", "-b:v", "0",
    ];
    const first = renderMp4({ format: "webm" }, "drain");
    await first.result;
    const second = renderMp4({ format: "webm", deterministic: true }, "drain");
    await second.result;

    expect(ffmpegInvocations()).toEqual([
      [...inputArgs(100, 100, 30), ...base, "-row-mt", "1", first.path],
      [...inputArgs(100, 100, 30), ...base, "-threads", "1", "-row-mt", "0", second.path],
    ]);
  });

  it("gif keeps its arguments and reports the gif codec", async () => {
    const { path, result } = renderMp4({ format: "gif" }, "drain");
    const res = await result;

    const [argv] = ffmpegInvocations();
    expect(argv.slice(0, 11)).toEqual(inputArgs(100, 100, 30));
    expect(argv).toContain("-filter_complex");
    expect(argv[argv.length - 1]).toBe(path);
    expect(res.encoder).toBe("gif");
  });

  const badOptions: [Partial<OutputConfig>, RegExp][] = [
    [{ preset: "turbo" }, /Invalid output\.preset "turbo".*veryfast/],
    [{ nvencPreset: "p9" }, /Invalid output\.nvencPreset "p9".*p1, p2/],
    [{ encoder: "qsv" as unknown as "auto" }, /Invalid output\.encoder "qsv".*auto, nvenc, x264/],
    [{ encoder: "nvenc", deterministic: true }, /deterministic cannot be combined/],
  ];
  it.each(badOptions)("rejects bad options %j before starting ffmpeg", async (output, message) => {
    const { result } = renderMp4(output, "drain");

    await expect(result).rejects.toThrow(message);
    await expect(result).rejects.toBeInstanceOf(RiveRenderError);
    expect(ffmpegInvocations()).toEqual([]);
  });
});

describe("NVENC fallback", () => {
  it("redoes the render with x264 when NVENC fails, and reports why", async () => {
    const { path, result } = renderMp4(
      { encoder: "nvenc", duration: 0.5 },
      "fail-nvenc",
      120,
      { ffmpegPath: fakeFfmpeg }
    );
    const res = await result;

    expect(res.success).toBe(true);
    expect(res.encoder).toBe("libx264");
    expect(res.encoderFallback).toContain("refusing to encode with h264_nvenc");
    expect(res.frameCount).toBe(15);

    const codecs = ffmpegInvocations().map((argv) => argv[argv.indexOf("-c:v") + 1]);
    expect(codecs).toEqual(["h264_nvenc", "libx264"]);

    const video = ffprobeVideo(path);
    expect(video.codec_name).toBe("h264");
    expect(video.pix_fmt).toBe("yuv420p");
    expect(video.width).toBe(120);
    expect(video.height).toBe(120);
    expect(Number(video.nb_read_frames)).toBe(15);
  });

  it("names both failures when the x264 attempt fails too", async () => {
    const { result } = renderMp4({ encoder: "nvenc" }, "fail-all");

    const err = await result.then(
      () => null,
      (e: unknown) => e
    );
    expect(err).toBeInstanceOf(RiveRenderError);
    const message = (err as RiveRenderError).message;
    expect(message).toContain("h264_nvenc failed");
    expect(message).toContain("libx264 fallback failed");
    expect(message).toContain("refusing to encode with libx264");
  });

  it("does not retry an x264 failure", async () => {
    const { result } = renderMp4({ encoder: "x264" }, "fail-all");

    await expect(result).rejects.toThrow(/ffmpeg exited with status 1 during video encoding/);
    expect(ffmpegInvocations()).toHaveLength(1);
  });

  it("does not retry when the render itself fails, and stops ffmpeg", async () => {
    // The artboard is looked up after ffmpeg has started; the abandoned ffmpeg is waiting for
    // frames that will never come and must be killed and reaped, not left behind.
    const { result } = renderMp4({ encoder: "nvenc" }, "drain", 100, { artboard: "no such artboard" });

    await expect(result).rejects.toThrow(/artboard/i);
    expect(ffmpegInvocations()).toHaveLength(1);

    const pid = Number(readFileSync(pidFile, "utf8"));
    expect(isAlive(pid)).toBe(false);
  });
});

describe("streaming", () => {
  it("renders a longer mp4 with the right frame count", async () => {
    const { path, result } = renderMp4({ duration: 4 }, "forward", 200, { ffmpegPath: realFfmpeg });
    const res = await result;

    expect(res.frameCount).toBe(120);
    expect(res.encoder).toBe("libx264");
    const video = ffprobeVideo(path);
    expect(video.codec_name).toBe("h264");
    expect(video.pix_fmt).toBe("yuv420p");
    expect(video.width).toBe(200);
    expect(video.height).toBe(200);
    expect(Number(video.nb_read_frames)).toBe(120);
  });
});

describe("device and encoder selection (--select-device)", () => {
  interface Selection {
    success: boolean;
    icd: string;
    icdFile?: string;
    encoder: string;
  }

  function select(
    config: Record<string, unknown>,
    env: Record<string, string | undefined>
  ): Selection {
    const merged: NodeJS.ProcessEnv = { ...process.env };
    delete merged.VK_ICD_FILENAMES;
    delete merged.VK_DRIVER_FILES;
    delete merged.RIVE_RENDER_NVIDIACTL_PATH;
    for (const [key, value] of Object.entries(env)) {
      if (value === undefined) delete merged[key];
      else merged[key] = value;
    }
    const run = spawnSync(resolveBinary(), ["--select-device"], {
      input: JSON.stringify({
        rivFile: BASKETBALL_RIV,
        output: { format: "mp4", path: join(work, "unused.mp4"), duration: 1 },
        ...config,
      }),
      env: merged,
      encoding: "utf8",
    });
    expect(run.status).toBe(0);
    const line = run.stdout.split("\n").find((l) => l.trimStart().startsWith("{"));
    return JSON.parse(line as string) as Selection;
  }

  let deviceNode: string;
  const missingNode = () => join(work, "missing-nvidiactl");

  beforeAll(() => {
    deviceNode = join(work, "nvidiactl");
    writeFileSync(deviceNode, "");
  });

  it("an NVIDIA device node selects the nvidia ICD and NVENC", () => {
    const tmp = mkdtempSync(join(work, "tmp-"));
    const sel = select({}, { RIVE_RENDER_NVIDIACTL_PATH: deviceNode, TMPDIR: tmp });

    if (IS_MAC) {
      expect(sel.icd).toBe("metal");
      expect(sel.encoder).toBe("libx264");
      return;
    }
    expect(sel.icd).toBe("nvidia");
    expect(sel.encoder).toBe("h264_nvenc");
    const bundled = join(dirname(resolveBinary()), "nvidia_icd.json");
    expect(sel.icdFile).toBe(
      existsSync(bundled) ? bundled : join(tmp, `rive-render-nvidia-icd-${process.getuid?.()}.json`)
    );
  });

  it("no device node means the GPU is not used: x264", () => {
    const sel = select({}, { RIVE_RENDER_NVIDIACTL_PATH: missingNode() });

    expect(sel.icd).not.toBe("nvidia");
    expect(sel.encoder).toBe("libx264");
    if (IS_MAC) expect(sel.icd).toBe("metal");
  });

  it("a VK_ICD_FILENAMES set by the caller is left alone", () => {
    const sel = select({}, {
      RIVE_RENDER_NVIDIACTL_PATH: deviceNode,
      VK_ICD_FILENAMES: "/opt/vendor/custom_icd.json",
    });

    expect(sel.icd).toBe(IS_MAC ? "metal" : "caller");
    expect(sel.encoder).toBe(IS_MAC ? "libx264" : "h264_nvenc");
  });

  it("a caller ICD that is SwiftShader does not get NVENC", () => {
    const sel = select({}, {
      RIVE_RENDER_NVIDIACTL_PATH: deviceNode,
      VK_ICD_FILENAMES: "/opt/swiftshader/vk_swiftshader_icd.json",
    });

    expect(sel.icd).toBe(IS_MAC ? "metal" : "caller");
    expect(sel.encoder).toBe("libx264");
  });

  it("swiftshader: true selects SwiftShader and x264 even with a device node", () => {
    const sel = select({ swiftshader: true }, { RIVE_RENDER_NVIDIACTL_PATH: deviceNode });

    expect(sel.icd).toBe(IS_MAC ? "metal" : "swiftshader");
    expect(sel.encoder).toBe("libx264");
  });

  it("deterministic and encoder x264 both force x264 even with a device node", () => {
    const env = { RIVE_RENDER_NVIDIACTL_PATH: deviceNode };
    const out = { format: "mp4", path: join(work, "unused.mp4"), duration: 1 };

    expect(select({ output: { ...out, deterministic: true } }, env).encoder).toBe("libx264");
    expect(select({ output: { ...out, encoder: "x264" } }, env).encoder).toBe("libx264");
  });

  it("webm and gif report their own codec", () => {
    const env = { RIVE_RENDER_NVIDIACTL_PATH: deviceNode };
    const out = { path: join(work, "unused"), duration: 1 };

    expect(select({ output: { ...out, format: "webm" } }, env).encoder).toBe("libvpx-vp9");
    expect(select({ output: { ...out, format: "gif" } }, env).encoder).toBe("gif");
  });
});
