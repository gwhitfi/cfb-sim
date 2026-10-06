import { describe, expect, it } from "vitest";
import {
  cannotCompare, compare, compareCsv, delta, deltaAdvantage, deltaSpan, inA, readDelta, settingsDiff, sharedShown, sortCompared,
} from "./compare";
import { ResultGrid, metricByKey, type CellStats, type MatrixInfo, type MatrixSettings } from "./results";
import { defenseRows, playRows, shown, totalRows, NO_FILTERS } from "./table";

const stats = (complete: number, over: Partial<CellStats> = {}): CellStats => ({
  plays: 100, complete, picks: 0, sacks: 0, scrambles: 0, success: 50, attempts: 100, pressured: 20, fumbles_lost: 0,
  gaining: 100, gained_yards: 600, gained_yards_sq: 5000, completed_yards: 600, yac: 200,
  nonpositive: 10, ten_plus: 30, twenty_plus: 10, ...over,
});

// A: smash, mesh vs c2, c3, cover0. B (in another order): mesh, smash, verts vs c3, c2, quarters.
const infoA: MatrixInfo = {
  offense: "a", defense: "b", runs: 100, seed: 1,
  rows: [{ id: "smash", name: "Smash", type: "pass" }, { id: "mesh", name: "Mesh", type: "pass" }],
  cols: [{ id: "c2", name: "Cover 2" }, { id: "c3", name: "Cover 3" }, { id: "c0", name: "Cover 0" }],
};
const infoB: MatrixInfo = {
  offense: "a", defense: "b", runs: 100, seed: 2,
  rows: [{ id: "mesh", name: "Mesh", type: "pass" }, { id: "smash", name: "Smash", type: "pass" }, { id: "verts", name: "Verticals", type: "pass" }],
  cols: [{ id: "c3", name: "Cover 3" }, { id: "c2", name: "Cover 2" }, { id: "qtr", name: "Quarters" }],
};

// Completions, by play and defense id.
const A: Record<string, number> = { "smash:c2": 60, "smash:c3": 50, "smash:c0": 0, "mesh:c2": 40, "mesh:c3": 40, "mesh:c0": 100 };
const B: Record<string, number> = { "smash:c2": 70, "smash:c3": 70, "mesh:c2": 41, "mesh:c3": 39 };

function grid(info: MatrixInfo, complete: Record<string, number>, settings: MatrixSettings = {}): ResultGrid {
  const g = new ResultGrid(info, settings);
  info.rows.forEach((p, r) => info.cols.forEach((d, c) => g.add({ row: r, col: c, stats: stats(complete[`${p.id}:${d.id}`] ?? 50) })));
  return g;
}

const completion = metricByKey("completion");
const cmp = () => compare(grid(infoA, A), grid(infoB, B));

describe("matching", () => {
  it("matches plays and defenses by id and lists what only one run has", () => {
    const c = cmp();
    expect([...c.rowInA]).toEqual([[0, 1], [1, 0]]);
    expect([...c.colInA]).toEqual([[0, 1], [1, 0]]);
    expect(c.onlyA).toEqual({ plays: [], defs: ["Cover 0"] });
    expect(c.onlyB).toEqual({ plays: ["Verticals"], defs: ["Quarters"] });
  });

  it("shows only what both have, and maps B's rows to A's", () => {
    const c = cmp();
    const vis = sharedShown(c, shown(infoB, NO_FILTERS));
    expect(vis).toEqual({ rows: [0, 1], cols: [0, 1] });
    const mesh = playRows(c.b, vis)[0]!;
    expect(inA(c, mesh)).toMatchObject({ rows: [1], cols: [1, 0], row: 1 });
    const meshVsC2 = defenseRows(c.b, 0, vis)[1]!;
    expect(inA(c, meshVsC2)).toMatchObject({ rows: [1], cols: [0], row: 1, col: 0 });
  });

  it("totals both sides over the shared defenses only", () => {
    const c = cmp();
    const vis = sharedShown(c, shown(infoB, NO_FILTERS));
    const smash = playRows(c.b, vis)[1]!;
    const x = readDelta(c, completion, smash);
    expect(x.a!.value).toBeCloseTo(110 / 200); // not counting Cover 0
    expect(x.b!.value).toBeCloseTo(140 / 200);
    expect(x.d).toBeCloseTo(0.15);
    const all = totalRows(c.b, vis).at(-1)!;
    expect(readDelta(c, completion, all).a!.value).toBeCloseTo(190 / 400);
  });

  it("can't compare a play run with a kick run", () => {
    expect(cannotCompare(grid(infoA, A), grid({ ...infoB, kind: "kicks" }, B))).toMatch(/can't be compared/);
    expect(cannotCompare(grid(infoA, A), grid(infoB, B))).toBeNull();
  });
});

describe("delta", () => {
  it("is B − A, within the noise when smaller than the difference's 95% interval", () => {
    const x = delta({ value: 0.5, se: 0.03 }, { value: 0.56, se: 0.04 });
    expect(x.d).toBeCloseTo(0.06);
    expect(x.noise).toBeCloseTo(1.96 * 0.05);
    expect(x.withinNoise).toBe(true);
    expect(delta({ value: 0.5, se: 0.03 }, { value: 0.7, se: 0.04 }).withinNoise).toBe(false);
  });

  it("can't tell the noise without both spreads, and has nothing without both readings", () => {
    expect(delta({ value: 10, se: null }, { value: 12, se: null })).toMatchObject({ d: 2, noise: null, withinNoise: false });
    expect(delta(null, { value: 1, se: 0 }).d).toBeNull();
  });

  it("colors toward the side it favors, full at the widest Δ", () => {
    expect(deltaAdvantage(completion, 0.1, 0.2)).toBeCloseTo(0.5);
    expect(deltaAdvantage(metricByKey("int"), 0.1, 0.1)).toBeCloseTo(-1); // more picks favor the defense
    expect(deltaSpan([delta({ value: 1, se: 0 }, { value: 3, se: 0 }), delta({ value: 1, se: 0 }, { value: 0, se: 0 }), delta(null, null)])).toBe(2);
  });

  it("sorts rows by A, B, or Δ", () => {
    const c = cmp();
    const rows = playRows(c.b, sharedShown(c, shown(infoB, NO_FILTERS)));
    expect(sortCompared(c, rows, completion, "d", "desc").map((r) => r.label)).toEqual(["Smash", "Mesh"]);
    expect(sortCompared(c, rows, completion, "a", "asc").map((r) => r.label)).toEqual(["Mesh", "Smash"]);
  });
});

describe("settings", () => {
  it("lists what differs, and whether it's still the same test", () => {
    const a = grid(infoA, A, { yard_line: 25, huddle: false });
    const b = grid({ ...infoB, runs: 200, defense: "c" }, B, { yard_line: 40, huddle: false });
    expect(settingsDiff(a, b)).toEqual([
      { label: "runs", a: "100 runs per cell", b: "200 runs per cell", sameTest: true },
      { label: "seed", a: "seed 1", b: "seed 2", sameTest: true },
      { label: "yard line", a: "yard line 25", b: "yard line 40", sameTest: false },
      { label: "defense", a: "b", b: "c", sameTest: false },
    ]);
  });
});

describe("export", () => {
  it("writes A, B, and Δ for each metric", () => {
    const c = cmp();
    const vis = sharedShown(c, shown(infoB, NO_FILTERS));
    const csv = compareCsv(c, [completion], [playRows(c.b, vis)[1]!]);
    expect(csv).toBe("Play,Defense,Completion (of attempts) A,Completion (of attempts) B,Completion (of attempts) Δ\nSmash,All shown,0.5500,0.7000,0.1500\n");
  });
});
