import { mkdirSync, readFileSync, unlinkSync } from "fs";
import { resolve } from "path";
import {
  RiveRenderer,
  RiveRenderError,
  type RenderResult,
  type RiveRenderConfig,
} from "../index";

// Coverage for renderer behaviour that depends on rive-runtime APIs adopted in
// the v0.1.480 bump: the internal FileAssetLoader (embedded-asset overrides),
// font + trigger view-model properties, trigger state-machine inputs, artboard
// auto-sizing and loud failures for unknown view model names.
//
// These tests assert render *deltas* (A differs from B / A equals B) rather
// than committed snapshots, so they hold on both macOS/Metal and
// Linux/SwiftShader.

const FIXTURES = resolve(__dirname, "..", "..", "..", "test", "fixtures");
const ASSET_LOAD_CHECK_RIV = resolve(FIXTURES, "asset_load_check.riv");
const DATA_BINDING_RIV = resolve(FIXTURES, "data_binding_test.riv");
const SAMPLE_JPEG = resolve(FIXTURES, "picture-47982.jpeg");
// data_bind_font_test.riv, kablammo.ttf and state_machine_triggers.riv are
// committed from rive-app/rive-runtime (tests/unit_tests/assets/), where they
// back data_binding_fonts_test.cpp and state_machine_test.cpp.
//   - data_bind_font_test.riv: `ViewModel1` with `fontSource` / `fontProperty`
//     asset-font properties; `fontProperty` drives a visible "text" run.
//   - state_machine_triggers.riv: artboard `main`, state machine input
//     `Trigger 1` (trigger) that animates the box.
const FONT_RIV = resolve(FIXTURES, "data_bind_font_test.riv");
const KABLAMMO_TTF = resolve(FIXTURES, "kablammo.ttf");
const SM_TRIGGERS_RIV = resolve(FIXTURES, "state_machine_triggers.riv");
const TMP = "/tmp/rive-features-work";

const cli = new RiveRenderer();

beforeAll(() => {
  mkdirSync(TMP, { recursive: true });
});

async function renderPng(
  config: Omit<RiveRenderConfig, "screenshot">,
  timestamp = 0
): Promise<{ png: Buffer; result: RenderResult }> {
  const path = `${TMP}/f-${Date.now()}-${Math.random().toString(36).slice(2)}.png`;
  const result = await cli.render({ ...config, screenshot: { path, timestamp } });
  const png = readFileSync(path);
  unlinkSync(path);
  return { png, result };
}

// PNG IHDR: width/height are big-endian uint32s at byte offsets 16 and 20.
function pngSize(png: Buffer): { width: number; height: number } {
  return { width: png.readUInt32BE(16), height: png.readUInt32BE(20) };
}

describe("Asset overrides (internal FileAssetLoader)", () => {
  // asset_load_check.riv embeds `cat` (uniqueName `cat-45019`). rive-runtime
  // >= v0.1.4xx no longer lets command-queue global assets replace embedded
  // bytes; rive-render's own loader keeps the historical behaviour.
  const base = { rivFile: ASSET_LOAD_CHECK_RIV, width: 400, height: 400 };

  it("a uniqueName key replaces an embedded image", async () => {
    const { png: plain } = await renderPng(base);
    const { png: swapped } = await renderPng({
      ...base,
      assets: { images: { "cat-45019.jpg": SAMPLE_JPEG } },
    });
    expect(swapped.equals(plain)).toBe(false);
  });

  it("a bare asset name does not replace an embedded image", async () => {
    // Bare names only match referenced / CDN slots (unchanged since v1).
    const { png: plain } = await renderPng(base);
    const { png: named } = await renderPng({
      ...base,
      assets: { images: { cat: SAMPLE_JPEG } },
    });
    expect(named.equals(plain)).toBe(true);
  });
});

describe("Font view-model property", () => {
  it("binding a font changes the rendered text", async () => {
    const { png: plain } = await renderPng({ rivFile: FONT_RIV, width: 250, height: 250 });
    const { png: withFont } = await renderPng({
      rivFile: FONT_RIV,
      width: 250,
      height: 250,
      viewModelData: {
        properties: { fontProperty: { type: "font", value: KABLAMMO_TTF } },
      },
    });
    expect(withFont.equals(plain)).toBe(false);
  });

  it("rejects a missing font path", async () => {
    await expect(
      renderPng({
        rivFile: FONT_RIV,
        width: 100,
        height: 100,
        viewModelData: {
          properties: { fontProperty: { type: "font", value: "/nonexistent/font.ttf" } },
        },
      })
    ).rejects.toThrow(RiveRenderError);
  });
});

describe("Triggers", () => {
  it("a view-model trigger property fires", async () => {
    // data_binding_test.riv `artboard-2` binds `vm2.trigger-prop`; firing it
    // moves the box. The control binds the same view model with a no-op
    // trigger path so the only difference is the fired trigger.
    const base = {
      rivFile: DATA_BINDING_RIV,
      artboard: "artboard-2",
      width: 200,
      height: 200,
    };
    const { png: control } = await renderPng(
      {
        ...base,
        viewModelData: {
          viewModel: "vm2",
          properties: { "no-such-trigger": { type: "trigger" } },
        },
      },
      1
    );
    const { png: fired } = await renderPng(
      {
        ...base,
        viewModelData: {
          viewModel: "vm2",
          properties: { "trigger-prop": { type: "trigger" } },
        },
      },
      1
    );
    expect(fired.equals(control)).toBe(false);
  });

  it("`true` on a state-machine trigger input fires it", async () => {
    const base = { rivFile: SM_TRIGGERS_RIV, width: 200, height: 200 };
    const { png: idle } = await renderPng(base, 0.5);
    const { png: fired } = await renderPng(
      { ...base, stateMachineInputs: { "Trigger 1": true } },
      0.5
    );
    expect(fired.equals(idle)).toBe(false);
  });
});

describe("Artboard auto-size", () => {
  it("uses the artboard size when width/height are omitted", async () => {
    // asset_load_check.riv's default artboard is 1000x800.
    const { png, result } = await renderPng({ rivFile: ASSET_LOAD_CHECK_RIV });
    expect(result).toMatchObject({ width: 1000, height: 800 });
    expect(pngSize(png)).toEqual({ width: 1000, height: 800 });
  });

  it("keeps the artboard aspect ratio when only width is given", async () => {
    const { png, result } = await renderPng({ rivFile: ASSET_LOAD_CHECK_RIV, width: 500 });
    expect(result).toMatchObject({ width: 500, height: 400 });
    expect(pngSize(png)).toEqual({ width: 500, height: 400 });
  });

  it("reports explicit sizes unchanged", async () => {
    const { result } = await renderPng({ rivFile: ASSET_LOAD_CHECK_RIV, width: 320, height: 240 });
    expect(result).toMatchObject({ width: 320, height: 240 });
  });
});

describe("View model lookup errors", () => {
  const base = { rivFile: DATA_BINDING_RIV, artboard: "artboard-1", width: 100, height: 100 };
  const properties = { text: { type: "string" as const, value: "x" } };

  it("rejects an unknown view model name with the native error", async () => {
    await expect(
      renderPng({ ...base, viewModelData: { viewModel: "noSuchVM", properties } })
    ).rejects.toThrow("View model not found: noSuchVM");
  });

  it("rejects an unknown instance name with the native error", async () => {
    await expect(
      renderPng({
        ...base,
        viewModelData: { viewModel: "vm1", instance: "noSuchInstance", properties },
      })
    ).rejects.toThrow("View model instance not found: noSuchInstance");
  });

  it("surfaces the native error message for a bad artboard", async () => {
    await expect(
      renderPng({ rivFile: DATA_BINDING_RIV, artboard: "nope", width: 100, height: 100 })
    ).rejects.toThrow(/Artboard not found: nope/);
  });
});
