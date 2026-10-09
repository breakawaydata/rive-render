import { mkdirSync, readFileSync, unlinkSync } from "fs";
import { resolve } from "path";
import { toMatchImageSnapshot } from "jest-image-snapshot";
import {
  RiveRenderer,
  RiveRenderError,
  type RenderResult,
  type RiveRenderConfig,
} from "../index";

expect.extend({ toMatchImageSnapshot });

// "Rive on Rive": an artboard of a second .riv bound into an artboard-typed
// view model property of the main file, with the nested view model data.
//
// databind_external_artboard_main.riv and databind_external_artboard_child.riv
// are committed from rive-app/rive-runtime (tests/unit_tests/assets/, MIT),
// where they back the "Data bind external artboard" silver test.
//   - main: artboard `Main`, view model `Main` with an artboard property `ab`
//     (empty until bound) hosting the external artboard.
//   - child: artboard `ExternalChild`, view model `Child` with a `label`
//     string rendered as text.
// The other fixtures act as "second files" for the cases the child cannot
// cover: teststatemachine.riv animates (a nested state machine), and
// data_binding_images_test.riv has an artboard `sub_1` with an image property.
//
// Most assertions are render deltas (A differs from B / A equals B), which
// hold on both macOS/Metal and Linux/SwiftShader; the snapshots pin one
// representative frame each.

const FIXTURES = resolve(__dirname, "..", "..", "..", "test", "fixtures");
const MAIN_RIV = resolve(FIXTURES, "databind_external_artboard_main.riv");
const CHILD_RIV = resolve(FIXTURES, "databind_external_artboard_child.riv");
const SM_RIV = resolve(FIXTURES, "teststatemachine.riv");
const IMAGES_RIV = resolve(FIXTURES, "data_binding_images_test.riv");
const SAMPLE_JPEG = resolve(FIXTURES, "picture-47982.jpeg");
// A second, different image: any committed reference PNG will do.
const OTHER_IMAGE = resolve(
  __dirname,
  "__image_snapshots__",
  "basketball-t0-400x400.png"
);
const TMP = "/tmp/rive-artboard-binding-work";

const cli = new RiveRenderer();

beforeAll(() => {
  mkdirSync(TMP, { recursive: true });
});

type Config = Omit<RiveRenderConfig, "rivFile" | "screenshot">;

async function renderPng(config: Config, timestamp = 0): Promise<Buffer> {
  const path = `${TMP}/ab-${Date.now()}-${Math.random().toString(36).slice(2)}.png`;
  const result: RenderResult = await cli.render({
    rivFile: MAIN_RIV,
    ...config,
    screenshot: { path, timestamp },
  });
  expect(result.success).toBe(true);
  const png = readFileSync(path);
  unlinkSync(path);
  return png;
}

const CHILD_FILES = { child: { rivFile: CHILD_RIV } };

function bindChild(label?: string): Config {
  return {
    extraFiles: CHILD_FILES,
    viewModelData: {
      properties: {
        ab: {
          type: "artboard",
          file: "child",
          artboard: "ExternalChild",
          properties:
            label === undefined
              ? undefined
              : { label: { type: "string", value: label } },
        },
      },
    },
  };
}

describe("Artboard binding: nested view model data", () => {
  it("renders the bound artboard, and a nested string value shows in it", async () => {
    const unbound = await renderPng({});
    const labelled = await renderPng(bindChild("NESTED LABEL"));
    const other = await renderPng(bindChild("SOMETHING ELSE"));

    // The main file draws nothing for `ab` until an artboard is bound.
    expect(labelled.equals(unbound)).toBe(false);
    // The text comes from the nested instance, not from the artboard's authoring.
    expect(labelled.equals(other)).toBe(false);

    expect(labelled).toMatchImageSnapshot({
      failureThreshold: 0.001,
      failureThresholdType: "percent",
      customSnapshotIdentifier: "artboard-binding-nested-string-500x500",
    });
  });

  it("binds the nested artboard to an instance of its own file even with no properties", async () => {
    // Without an instance from the child's file the artboard would read the
    // main file's view models and lose its label.
    const defaults = await renderPng(bindChild());
    const labelled = await renderPng(bindChild("NESTED LABEL"));
    const unbound = await renderPng({});
    expect(defaults.equals(unbound)).toBe(false);
    expect(defaults.equals(labelled)).toBe(false);
  });

  it("is deterministic for identical payloads", async () => {
    const a = await renderPng(bindChild("SAME"));
    const b = await renderPng(bindChild("SAME"));
    expect(a.equals(b)).toBe(true);
  });

  it("the screenshot helper threads extraFiles through", async () => {
    const path = `${TMP}/helper-${Date.now()}.png`;
    await cli.screenshot(MAIN_RIV, {
      outputPath: path,
      width: 500,
      height: 500,
      ...bindChild("NESTED LABEL"),
    });
    const viaHelper = readFileSync(path);
    unlinkSync(path);
    const viaRender = await renderPng(bindChild("NESTED LABEL"));
    expect(viaHelper.equals(viaRender)).toBe(true);
  });
});

describe("Artboard binding: nested state machine", () => {
  const config: Config = {
    extraFiles: { anim: { rivFile: SM_RIV } },
    viewModelData: {
      properties: {
        ab: { type: "artboard", file: "anim", artboard: "New Artboard" },
      },
    },
  };

  it("frames at t=0 and t=1 differ: the nested state machine advances in the host", async () => {
    const t0 = await renderPng(config, 0);
    const t1 = await renderPng(config, 1);
    expect(t1.equals(t0)).toBe(false);
    expect(t0).toMatchImageSnapshot({
      failureThreshold: 0.001,
      failureThresholdType: "percent",
      customSnapshotIdentifier: "artboard-binding-nested-sm-t0-500x500",
    });
    expect(t1).toMatchImageSnapshot({
      failureThreshold: 0.001,
      failureThresholdType: "percent",
      customSnapshotIdentifier: "artboard-binding-nested-sm-t1-500x500",
    });
  });
});

describe("Artboard binding: nested image property", () => {
  const withImage = (properties?: Record<string, never>): Config => ({
    extraFiles: { imgs: { rivFile: IMAGES_RIV } },
    viewModelData: {
      properties: {
        ab: {
          type: "artboard",
          file: "imgs",
          artboard: "sub_1",
          viewModel: "sub_1",
          properties: properties ?? {
            sub_1_im: { type: "image", value: SAMPLE_JPEG },
          },
        },
      },
    },
  });

  it("an image bound inside the nested view model renders", async () => {
    const authored = await renderPng(withImage({}));
    const bound = await renderPng(withImage());
    expect(bound.equals(authored)).toBe(false);
    expect(bound).toMatchImageSnapshot({
      failureThreshold: 0.001,
      failureThresholdType: "percent",
      customSnapshotIdentifier: "artboard-binding-nested-image-500x500",
    });
  });
});

describe("Artboard binding: per-file asset overrides", () => {
  // data_binding_images_test.riv's `static` artboard shows images embedded in
  // that file (`funkos_N`, uniqueName `funkos_N-<id>`). The main file has no
  // assets, so a main-file override under the same key must not reach it, and
  // an extra-file override must not depend on the main file's table.
  const FUNKOS = [
    "funkos_1-85406",
    "funkos_2-85404",
    "funkos_3-85408",
    "funkos_4-85407",
    "funkos_5-85405",
    "funkos_6-85409",
    "funkos_7-85410",
    "funkos_8-85412",
    "funkos_9-85411",
  ];
  const overrides = (image: string) => ({
    images: Object.fromEntries(FUNKOS.map((name) => [name, image])),
  });
  const bind = (extraAssets?: { images: Record<string, string> }): Config => ({
    extraFiles: {
      card: { rivFile: IMAGES_RIV, assets: extraAssets },
    },
    viewModelData: {
      properties: {
        ab: { type: "artboard", file: "card", artboard: "static" },
      },
    },
  });

  it("the extra file's override replaces the extra file's own asset", async () => {
    const plain = await renderPng(bind());
    const overridden = await renderPng(bind(overrides(SAMPLE_JPEG)));
    expect(overridden.equals(plain)).toBe(false);
  });

  it("a main-file override with the same name does not reach the extra file", async () => {
    const plain = await renderPng(bind());
    const mainOverride = await renderPng({
      ...bind(),
      assets: overrides(OTHER_IMAGE),
    });
    expect(mainOverride.equals(plain)).toBe(true);
  });

  it("each file keeps its own override when both name the same asset", async () => {
    const extraOnly = await renderPng(bind(overrides(SAMPLE_JPEG)));
    const both = await renderPng({
      ...bind(overrides(SAMPLE_JPEG)),
      assets: overrides(OTHER_IMAGE),
    });
    expect(both.equals(extraOnly)).toBe(true);

    // And the extra file really uses ITS image, not the main file's.
    const swapped = await renderPng(bind(overrides(OTHER_IMAGE)));
    expect(swapped.equals(extraOnly)).toBe(false);
  });
});

describe("Artboard binding: errors", () => {
  const render = (
    prop: Record<string, unknown>,
    extra: Config["extraFiles"] = CHILD_FILES,
    path = "ab"
  ) =>
    cli.render({
      rivFile: MAIN_RIV,
      screenshot: { path: `${TMP}/err-${Date.now()}.png` },
      extraFiles: extra,
      viewModelData: {
        properties: {
          [path]: { type: "artboard", ...prop } as never,
        },
      },
    });

  it("rejects an unknown extraFiles alias, naming it", async () => {
    const err = await render({ file: "nope", artboard: "ExternalChild" }).catch(
      (e: unknown) => e
    );
    expect(err).toBeInstanceOf(RiveRenderError);
    expect((err as Error).message).toMatch(/unknown file 'nope'/);
    expect((err as Error).message).toMatch(/extraFiles/);
  });

  it("rejects an unknown artboard, naming it and the file", async () => {
    await expect(
      render({ file: "child", artboard: "NoSuchArtboard" })
    ).rejects.toThrow(/artboard 'NoSuchArtboard' not found in file 'child'/);
  });

  it("rejects an unknown view model, naming it and the file", async () => {
    await expect(
      render({
        file: "child",
        artboard: "ExternalChild",
        viewModel: "NoSuchViewModel",
      })
    ).rejects.toThrow(/view model 'NoSuchViewModel' not found in file 'child'/);
  });

  it("rejects a path with no artboard property", async () => {
    await expect(
      render({ file: "child", artboard: "ExternalChild" }, CHILD_FILES, "nope")
    ).rejects.toThrow(/No artboard property at path 'nope'/);
  });

  it("rejects a path that is a different property type", async () => {
    // `child` is a view-model property of the main file, not an artboard one.
    await expect(
      render({ file: "child", artboard: "ExternalChild" }, CHILD_FILES, "child")
    ).rejects.toThrow(/No artboard property at path 'child'/);
  });

  it("rejects a binding with no file or artboard", async () => {
    await expect(render({ artboard: "ExternalChild" })).rejects.toThrow(
      /requires both "file" and "artboard"/
    );
    await expect(render({ file: "child" })).rejects.toThrow(
      /requires both "file" and "artboard"/
    );
  });

  it("rejects an extra .riv file that does not exist", async () => {
    await expect(
      render(
        { file: "child", artboard: "ExternalChild" },
        { child: { rivFile: "/nonexistent/extra.riv" } }
      )
    ).rejects.toThrow(/Failed to open extra \.riv file 'child'/);
  });

  it("rejects properties for an extra file that has no view model", async () => {
    await expect(
      render(
        {
          file: "anim",
          artboard: "New Artboard",
          properties: { x: { type: "string", value: "y" } },
        },
        { anim: { rivFile: SM_RIV } }
      )
    ).rejects.toThrow(/has no view model/);
  });
});
