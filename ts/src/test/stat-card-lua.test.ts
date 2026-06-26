import { readFileSync, mkdirSync, unlinkSync } from "fs";
import { resolve } from "path";
import { toMatchImageSnapshot } from "jest-image-snapshot";
import { RiveRenderer, type PropertyValue, type ListItemConfig } from "../index";

expect.extend({ toMatchImageSnapshot });

const FIXTURES = resolve(__dirname, "..", "..", "..", "test", "fixtures");
// stat_card_lua_list.riv is BreakAway's `stat-card.v4.riv`. Its `Stat Card`
// artboard runs an embedded Luau script, `StatListAutoCardType`, that watches
// the bound `stat_list` and sets a `card_type` enum (Key / Multiple / Many)
// which selects the card's background variant. The script's bind -> rebind
// cycle settles by t=1.0s.
//
// Regression guard for the script <-> view-model property bridge. A
// rive-runtime change at runtime-v0.1.107 (v0.1.106 is the last good release;
// bisected by build) reworked how scripted view-model properties resolve, and
// the script's nested-object lookups then returned nil. `sync` died with
// `attempt to index nil with 'statItems1_N'` for any list with >= 2 items, so
// `card_type` was never set and every multi-stat card fell back to the wrong
// default background. NB this is NOT the Luau VM bump: rive_0_35 -> rive_0_36
// (runtime-v0.1.94) renders these cards correctly and byte-identically.
// Single-item ("Key") cards don't touch the statItems slots, which is why a
// list-count sweep is the discriminating test. These snapshots are rendered
// against a runtime that runs the script correctly; a future runtime bump that
// re-breaks the bridge will diverge the >= 2-item snapshots while leaving the
// 1-item one matching.
const STAT_CARD_RIV = resolve(FIXTURES, "stat_card_lua_list.riv");
const TMP = "/tmp/rive-statcard-work";

const cli = new RiveRenderer();

beforeAll(() => {
  mkdirSync(TMP, { recursive: true });
});

// Football-stat row templates. The first row is highlighted; the script keys
// the `card_type` off the list length, so only the count matters for the
// background-variant selection this test guards.
const STAT_TEMPLATES: ReadonlyArray<{ label: string; value: string }> = [
  { label: "RUSH YDS", value: "123" },
  { label: "PASS YDS", value: "234" },
  { label: "TD", value: "5" },
  { label: "TACKLES", value: "12" },
  { label: "INT", value: "3" },
  { label: "FUMBLES", value: "1" },
  { label: "SACKS", value: "2" },
  { label: "CATCHES", value: "8" },
];

const buildStatList = (count: number): ListItemConfig[] =>
  Array.from({ length: count }, (_, i) => ({
    viewModel: "StatItem",
    properties: {
      label: { type: "string", value: STAT_TEMPLATES[i].label },
      value: { type: "string", value: STAT_TEMPLATES[i].value },
      highlighted: { type: "boolean", value: i === 0 },
    },
  }));

// Top-level scalar bindings on `Stat Card`'s `ViewModel1` / `Instance`. Logos
// are referenced (CDN/image) assets; this regression test keeps the fixture
// self-contained by leaving them off — the Luau-driven background variant
// (what the regression corrupts) does not depend on them.
const SCALAR_PROPERTIES: Record<string, PropertyValue> = {
  player_first_name: { type: "string", value: "Miranda" },
  player_last_name: { type: "string", value: "Treutel" },
  player_jersey_number: { type: "string", value: "49" },
  player_team_name: { type: "string", value: "Rapids U15 Boys" },
  score_team1_name: { type: "string", value: "Rapids U15 Boys" },
  score_team1_value: { type: "string", value: "8" },
  score_team2_name: { type: "string", value: "Test" },
  score_team2_value: { type: "string", value: "8" },
  month: { type: "string", value: "MAY" },
  day: { type: "string", value: "08" },
  year: { type: "string", value: "26" },
  show_team1_logo: { type: "boolean", value: false },
  show_team2_logo: { type: "boolean", value: false },
  show_jersey_number: { type: "boolean", value: true },
  show_location: { type: "boolean", value: true },
  show_event_name: { type: "boolean", value: true },
};

async function renderStatCard(statCount: number): Promise<Buffer> {
  const tmp = `${TMP}/stat-${statCount}-${Date.now()}.png`;
  await cli.render({
    rivFile: STAT_CARD_RIV,
    artboard: "Stat Card",
    stateMachine: "State Machine 1",
    width: 380 * 3,
    height: 532 * 3,
    // 1.0s lets the StatListAutoCardType Lua script complete its bind ->
    // rebind cycle before the frame is captured.
    screenshot: { path: tmp, timestamp: 1.0 },
    viewModelData: {
      viewModel: "ViewModel1",
      instance: "Instance",
      properties: {
        ...SCALAR_PROPERTIES,
        stat_list: { type: "list", value: buildStatList(statCount) },
      },
    },
  });
  const buf = readFileSync(tmp);
  unlinkSync(tmp);
  return buf;
}

describe("Luau stat-card list -> card_type (rive_0_36 regression guard)", () => {
  // Generous threshold on purpose: this guards a *categorical* failure (the
  // script picks the wrong card_type → a whole different background, ~38% of
  // pixels), not sub-pixel fidelity. 5% easily catches that while tolerating
  // any GPU/anti-aliasing variance between the macOS-Metal refs and Linux
  // SwiftShader CI.
  const snapshotConfig = {
    failureThreshold: 0.05,
    failureThresholdType: "percent" as const,
  };

  // 1 stat = "Key", 3 = "Multiple", 8 = "Many": one per authored card-type
  // bucket. Under the rive_0_36 regression the 3- and 8-stat renders collapse
  // to the wrong default background and these snapshots diverge.
  for (const count of [1, 3, 8]) {
    it(`renders the correct background variant for ${count} stat row${count === 1 ? "" : "s"}`, async () => {
      const image = await renderStatCard(count);
      expect(image).toMatchImageSnapshot({
        ...snapshotConfig,
        customSnapshotIdentifier: `stat-card-lua-${count}-stats`,
      });
    });
  }

  it("renders deterministically for the same list", async () => {
    const a = await renderStatCard(3);
    const b = await renderStatCard(3);
    expect(a.equals(b)).toBe(true);
  });
});
