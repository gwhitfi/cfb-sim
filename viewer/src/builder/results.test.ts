import { describe, expect, it } from "vitest";
import {
  ResultGrid, appliesTo, countOf, domainOf, formatDelta, formatValue, halfWidth, isNoisy, metricByKey, reading, settingsText, sum, titleOf, totalKind,
  type CellMessage, type CellStats, type KickStats, type MatrixInfo,
} from "./results";

const info: MatrixInfo = {
  offense: "a",
  defense: "b",
  runs: 10,
  seed: 1,
  rows: [
    { id: "smash", name: "Smash", type: "pass" },
    { id: "iz", name: "Inside Zone", type: "run" },
  ],
  cols: [
    { id: "c2", name: "Cover 2" },
    { id: "c3", name: "Cover 3" },
  ],
};

const stats = (over: Partial<CellStats> = {}): CellStats => ({
  plays: 10, complete: 6, picks: 1, sacks: 1, scrambles: 0, success: 5, pressured: 2, fumbles_lost: 0,
  gaining: 8, gained_yards: 60, gained_yards_sq: 500, completed_yards: 55, yac: 20,
  nonpositive: 1, ten_plus: 3, twenty_plus: 1, gains: { mean: 6 }, ...over,
});
const msg = (row: number, col: number, over: Partial<CellStats> = {}): CellMessage => ({ row, col, stats: stats(over) });
const grid = () => new ResultGrid(info, { offense: "a", defense: "b" });

describe("ResultGrid", () => {
  it("fills in as cells arrive, in any order, and counts each cell once", () => {
    const g = grid();
    expect(g.total).toBe(4);
    g.add(msg(1, 0));
    g.add(msg(0, 1));
    g.add(msg(0, 1, { plays: 20 })); // a repeat replaces, doesn't count twice
    expect(g.done).toBe(2);
    expect(g.stats(0, 1)!.plays).toBe(20);
    expect(g.stats(0, 0)).toBeNull();
  });
  it("ignores a cell outside the grid", () => {
    const g = grid();
    g.add(msg(5, 0));
    g.add(msg(0, 9));
    expect(g.done).toBe(0);
  });
  it("totals any rows and columns from the cells that are in", () => {
    const g = grid();
    expect(g.total_of([0, 1], [0, 1])).toBeNull();
    g.add(msg(0, 0));
    g.add(msg(0, 1, { plays: 30, gained_yards: 90 }));
    g.add(msg(1, 1, { plays: 5, gained_yards: 20 }));
    expect(g.total_of([0], [0, 1])!.plays).toBe(40); // a row
    expect(g.total_of([0, 1], [1])!.plays).toBe(35); // a column
    expect(g.total_of([0], [1])!.gained_yards).toBe(90); // one cell
    expect(g.total_of([0, 1], [0, 1])!.gained_yards).toBe(170);
    expect(g.total_of([1], [0])).toBeNull(); // not arrived
    expect(g.rowKind(1)).toBe("run");
  });
});

describe("sum", () => {
  it("adds numbers and drops tables and spreads", () => {
    const s = sum([stats(), stats()])!;
    expect(s.plays).toBe(20);
    expect(s).not.toHaveProperty("gains");
  });
});

describe("metrics", () => {
  const s = stats();
  const value = (key: string, kind: "pass" | "run" | "both" = "pass", st: CellStats | null = s) =>
    reading(metricByKey(key), st, kind)?.value ?? null;

  it("reads each value from the counts", () => {
    expect(value("ypp")).toBe(6);
    expect(value("completion")).toBeCloseTo(6 / 9); // of attempts: plays minus sacks and scrambles
    expect(value("int")).toBeCloseTo(1 / 9);
    expect(value("sack")).toBeCloseTo(0.1);
    expect(value("ypc")).toBeCloseTo(55 / 6);
    expect(value("yac")).toBeCloseTo(20 / 6);
    expect(value("nonpos")).toBeCloseTo(1 / 8); // of the plays that gained
    expect(value("ten")).toBeCloseTo(3 / 8);
  });
  it("reads completion and interceptions over the server's attempts", () => {
    // 2 scrambles: one sacked (also a sack), one that ended in a throw. The
    // server's 9 attempts count that throw; plays - sacks - scrambles (7) wouldn't.
    const scrambled = stats({ sacks: 2, scrambles: 2, attempts: 8 });
    expect(value("completion", "pass", scrambled)).toBeCloseTo(6 / 8);
    expect(value("int", "pass", scrambled)).toBeCloseTo(1 / 8);
    // A run saved before attempts were counted falls back to the old count.
    expect(value("completion", "pass", stats({ sacks: 2, scrambles: 2 }))).toBeCloseTo(6 / 6);
  });
  it("is blank for a pass metric on a run row, and for nothing to read", () => {
    expect(value("completion", "run")).toBeNull();
    expect(value("completion", "both")).toBeCloseTo(6 / 9);
    expect(value("ypp", "pass", stats({ plays: 0 }))).toBeNull();
    expect(value("ypp", "pass", null)).toBeNull();
    expect(value("ten", "pass", stats({ gaining: 0 }))).toBeNull();
  });
  it("knows how sure a rate is, and calls a thin cell noisy", () => {
    const m = metricByKey("completion");
    const thin = reading(m, stats({ plays: 10, complete: 6 }), "pass");
    const thick = reading(m, stats({ plays: 1000, complete: 600, sacks: 0 }), "pass");
    expect(halfWidth(thin)!).toBeGreaterThan(halfWidth(thick)!);
    expect(isNoisy(m, thin)).toBe(true);
    expect(isNoisy(m, thick)).toBe(false);
    expect(halfWidth(reading(metricByKey("ypc"), s, "pass"))).toBeNull(); // no spread to tell
    expect(isNoisy(metricByKey("ypc"), reading(metricByKey("ypc"), s, "pass"))).toBe(false);
  });
  it("tells the spread of yards per play from the sum of squares", () => {
    // 4 plays of 0, 0, 10, 10: mean 5, sample variance 33.3, se = sqrt(33.3 / 4)
    const r = reading(metricByKey("ypp"), stats({ plays: 4, gained_yards: 20, gained_yards_sq: 200 }), "pass")!;
    expect(r.value).toBe(5);
    expect(r.se!).toBeCloseTo(Math.sqrt(100 / 3 / 4));
  });
  it("formats values and differences", () => {
    expect(formatValue(metricByKey("ypp"), 6.04)).toBe("6.0");
    expect(formatValue(metricByKey("completion"), 0.6)).toBe("60%");
    expect(formatValue(metricByKey("sack"), 0.0666)).toBe("6.7%");
    expect(formatValue(metricByKey("ypp"), null)).toBe("–");
    expect(formatDelta(metricByKey("ypp"), 0.64)).toBe("+0.6");
    expect(formatDelta(metricByKey("ypp"), -1.234)).toBe("-1.2");
    expect(formatDelta(metricByKey("completion"), 0.042)).toBe("+4pt");
    expect(formatDelta(metricByKey("completion"), -0.001)).toBe("0"); // no "-0"
  });
});

describe("kick matrices", () => {
  const kickInfo: MatrixInfo = {
    kind: "kicks", kicking: "a", receiving: "b", runs: 10, seed: 1,
    rows: [{ id: "kickoff_deep", name: "Deep", type: "kickoff" }, { id: "punt_normal", name: "Punt", type: "punt" }, { id: "fg_kick", name: "FG", type: "field_goal" }],
    cols: [{ id: "kr", name: "KR", type: "kickoff_return" }, { id: "pr", name: "PR", type: "punt_return" }],
  };
  const kicks = (over: Partial<KickStats> = {}): KickStats => ({
    kicks: 100, returned: 60, touchdowns: 1, touchbacks: 30, fair_catches: 20, recovered_kicking: 0, made: 0, blocked: 0,
    distance: 6000, hang: 400, net_yards: 4000, returns: 61, return_yards: 1220, ...over,
  });
  const read = (key: string, kind: "kickoff" | "punt" | "field_goal" | "both", s: KickStats | null = kicks()) =>
    reading(metricByKey(key, "kicks"), s, kind)?.value ?? null;

  it("knows its domain, title, and kinds of row", () => {
    const g = new ResultGrid(kickInfo, { kind: "kicks", kicking: "a", receiving: "b" });
    expect(g.domain).toBe("kicks");
    expect(g.kinds()).toEqual(["kickoff", "punt", "field_goal"]);
    expect(g.rowKind(2)).toBe("field_goal");
    expect(titleOf(kickInfo)).toBe("a kicking vs b returning");
    expect(titleOf(info)).toBe("a offense vs b");
    expect(titleOf(info, (id) => id.toUpperCase())).toBe("A offense vs B");
  });

  it("puts a run's settings in a line", () => {
    expect(settingsText(info, { yard_line: 40, huddle: true, hash: "left" })).toBe("10 runs per cell · seed 1 · left hash · yard line 40 · huddle");
    expect(settingsText(info, {})).toBe("10 runs per cell · seed 1 · rotating hash · yard line 25 · no huddle");
    expect(settingsText(kickInfo, {})).toBe(`${kickInfo.runs} kicks per cell · seed ${kickInfo.seed} · rotating hash · the usual spot · return aggression 50`);
    expect(domainOf(info)).toBe("scrimmage"); // an older run has no kind
  });
  it("reads the kick numbers from the sums", () => {
    expect(read("net", "kickoff")).toBeCloseTo(40);
    expect(read("distance", "punt")).toBeCloseTo(60);
    expect(read("hang", "punt")).toBeCloseTo(4);
    expect(read("return", "kickoff")).toBeCloseTo(1220 / 61);
    expect(read("returned", "punt")).toBeCloseTo(0.61);
    expect(read("touchback", "kickoff")).toBeCloseTo(0.3);
    expect(read("faircatch", "punt")).toBeCloseTo(0.2);
  });
  it("means nothing for the wrong kind of kick, or no kicks", () => {
    expect(read("net", "field_goal")).toBeNull();
    expect(read("touchback", "punt")).toBeNull();
    expect(read("made", "kickoff")).toBeNull();
    expect(read("net", "kickoff", kicks({ kicks: 0 }))).toBeNull();
    expect(read("return", "kickoff", kicks({ returns: 0 }))).toBeNull();
  });
  it("reads a total over kickoffs and punts, but not field goals", () => {
    const m = metricByKey("net", "kicks");
    expect(appliesTo(m, "kickoff")).toBe(true);
    expect(appliesTo(m, "field_goal")).toBe(false);
    expect(totalKind(m)).toBe("both");
    expect(totalKind(metricByKey("made", "kicks"))).toBe("field_goal");
  });
  it("counts plays or kicks, and sums kick stats", () => {
    expect(countOf(kicks())).toBe(100);
    expect(countOf(stats())).toBe(10);
    const g = new ResultGrid(kickInfo, { kind: "kicks" });
    g.add({ row: 0, col: 0, stats: kicks(), applies: true });
    g.add({ row: 0, col: 1, stats: kicks({ kicks: 0 }), applies: false });
    expect((g.total_of([0], [0, 1]) as KickStats).kicks).toBe(100);
  });
});
