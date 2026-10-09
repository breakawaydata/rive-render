// Runs inside test/no-shell/Dockerfile: no /bin/sh, uid 1001, HOME "/".
// Renders MP4, WebM and GIF through the public API with ffmpeg auto-resolved,
// and checks the resolver's and the binary's error paths.
const fs = require("fs");
const os = require("os");
const { spawnSync } = require("child_process");
const { RiveRenderer } = require("/app/dist/index.js");
const { resolveFFmpeg } = require("/app/dist/ffmpeg-resolver.js");

const BINARY = "/app/rive_render";
const RIV = "/app/basketball.riv";
let failed = 0;
const check = (name, ok, detail) => {
  if (!ok) failed++;
  console.log(`${ok ? "PASS" : "FAIL"} ${name}${detail ? ` :: ${detail}` : ""}`);
};

(async () => {
  check("no shell in image", !fs.existsSync("/bin/sh") && !fs.existsSync("/usr/bin/sh"));
  console.log(`uid=${process.getuid()} homedir=${os.homedir()}`);

  try {
    const p = await resolveFFmpeg();
    check("resolver finds ffmpeg by scanning PATH", p === "/usr/bin/ffmpeg", p);
  } catch (e) {
    check("resolver finds ffmpeg by scanning PATH", false, e.message);
  }

  try {
    await resolveFFmpeg({ env: { PATH: "/nonexistent" } });
    check("resolver refuses an unwritable cache dir", false, "did not throw");
  } catch (e) {
    check(
      "resolver refuses an unwritable cache dir",
      e.message.includes("not writable") && e.message.includes("/.rive-render/ffmpeg"),
      e.message
    );
  }

  fs.mkdirSync("/tmp/out", { recursive: true });
  const cli = new RiveRenderer({ binaryPath: BINARY });
  const cases = [
    ["mp4", () => cli.renderVideo(RIV, { outputPath: "/tmp/out/a.mp4", format: "mp4", width: 320, height: 240, fps: 30, duration: 1 })],
    ["webm", () => cli.renderVideo(RIV, { outputPath: "/tmp/out/a.webm", format: "webm", width: 320, height: 240, fps: 30, duration: 1 })],
    ["gif", () => cli.renderGif(RIV, { outputPath: "/tmp/out/a.gif", width: 320, height: 240, fps: 10, duration: 1 })],
  ];
  for (const [kind, run] of cases) {
    try {
      const r = await run();
      const size = fs.statSync(r.outputPath).size;
      check(`renders ${kind} without a shell`, r.success && size > 0, `${r.frameCount} frames, ${size} bytes`);
    } catch (e) {
      check(`renders ${kind} without a shell`, false, e.message);
    }
  }

  const cfg = JSON.stringify({
    rivFile: RIV, width: 64, height: 64, ffmpegPath: "/usr/bin/ffmpeg",
    output: { path: "/nonexistent-dir/x.mp4", format: "mp4", fps: 10, duration: 0.3 },
  });
  const r = spawnSync(BINARY, [], { input: cfg, encoding: "utf8" });
  const line = (r.stdout || "").trim().split("\n").pop() || "";
  check("ffmpeg failure is reported with its stderr", /exited with status \d+.*stderr/s.test(line), line.slice(0, 200));

  console.log(failed ? `${failed} check(s) failed` : "all checks passed");
  process.exit(failed ? 1 : 0);
})();
