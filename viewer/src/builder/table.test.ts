import { describe, expect, it } from "vitest";
import { ResultGrid, metricByKey, type CellStats, type KickStats, type MatrixInfo } from "./results";
import {
  NO_FILTERS, baselines, defenseRows, filterGroups, hiddenCount, metricsShown, playRows, presetsFor, readRow, setShown, shown,
  sortRows, tableCsv, totalRows,
} from "./table";

const info: MatrixInfo = {
  offense: "a",
  defense: "b",
  runs: 10,
  seed: 1,
  rows: [
    { id: "smash", name: "Smash", type: "pass", formation: "gun_2x2", formation_name: "Gun 2x2" },
    { id: "iz", name: "Inside Zone", type: "run", formation: "gun_trips", formation_name: "Gun Trips" },
    { id: "mesh", name: "Mesh", type: "pass", formation: "gun_2x2", formation_name: "Gun 2x2" },
  ],
  cols: [
    { id: "c2", name: "Cover 2", formation: "nickel", formation_name: "Nickel" },
    { id: "c3", name: "Cover 3", formation: "base_43", formation_name: "4-3" },
  ],
};

const stats = (over: Partial<CellStats> = {}): CellStats => ({
  plays: 10, complete: 6, picks: 0, sacks: 0, scrambles: 0, success: 5, attempts: 10, pressured: 2, fumbles_lost: 0,
  gaining: 10, gained_yards: 60, gained_yards_sq: 500, completed_yards: 60, yac: 20,
  nonpositive: 1, ten_plus: 3, twenty_plus: 1, ...over,
});

// Completions: smash 6 and 8, mesh 2 and 4 (of 10 each); the run's cells carry none.
function grid(): ResultGrid {
  const g = new ResultGrid(info, { offense: "a", defense: "b" });
  g.add({ row: 0, col: 0, stats: stats({ complete: 6 }) });
  g.add({ row: 0, col: 1, stats: stats({ complete: 8 }) });
  g.add({ row: 1, col: 0, stats: stats({ complete: 0, attempts: 0, gained_yards: 40 }) });
  g.add({ row: 1, col: 1, stats: stats({ complete: 0, attempts: 0, gained_yards: 20 }) });
  g.add({ row: 2, col: 0, stats: stats({ complete: 2 }) });
  g.add({ row: 2, col: 1, stats: stats({ complete: 4 }) });
  return g;
}

const completion = metricByKey("completion");
const ypp = metricByKey("ypp");
const all = { rows: [0, 1, 2], cols: [0, 1] };

describe("filters", () => {
  it("show everything by default and hide by kind, play, and defense", () => {
    expect(shown(info, NO_FILTERS)).toEqual(all);
    expect(shown(info, { ...NO_FILTERS, kinds: ["run"] })).toEqual({ rows: [0, 2], cols: [0, 1] });
    expect(shown(info, { kinds: [], plays: ["mesh"], defs: ["c2"] })).toEqual({ rows: [0, 1], cols: [1] });
    expect(hiddenCount({ kinds: ["run"], plays: ["mesh"], defs: ["c2"] })).toBe(3);
  });

  it("hide and show a group of ids at once", () => {
    expect(setShown(["iz"], ["smash", "mesh"], false).sort()).toEqual(["iz", "mesh", "smash"]);
    expect(setShown(["iz", "smash", "mesh"], ["smash", "mesh"], true)).toEqual(["iz"]);
  });

  it("group plays and defenses by formation, in the run's order", () => {
    expect(filterGroups(info, "plays")).toEqual([
      { key: "gun_2x2", name: "Gun 2x2", items: [{ id: "smash", name: "Smash" }, { id: "mesh", name: "Mesh" }] },
      { key: "gun_trips", name: "Gun Trips", items: [{ id: "iz", name: "Inside Zone" }] },
    ]);
    expect(filterGroups(info, "defs").map((g) => g.name)).toEqual(["Nickel", "4-3"]);
  });

  it("put a run saved without formations in one group, and kicks by kind", () => {
    const old: MatrixInfo = { ...info, rows: info.rows.map(({ id, name, type }) => ({ id, name, type })) };
    expect(filterGroups(old, "plays")).toEqual([{ key: "", name: "Plays", items: old.rows.map(({ id, name }) => ({ id, name })) }]);
    const kicks: MatrixInfo = {
      kind: "kicks", runs: 5, seed: 1,
      rows: [{ id: "ko", name: "Deep", type: "kickoff" }, { id: "p", name: "Punt", type: "punt" }],
      cols: [{ id: "kr", name: "Middle", type: "kickoff_return" }, { id: "pr", name: "Return", type: "punt_return" }],
    };
    expect(filterGroups(kicks, "plays").map((g) => g.name)).toEqual(["Kickoffs", "Punts"]);
    expect(filterGroups(kicks, "defs").map((g) => g.name)).toEqual(["Kickoff returns", "Punt returns"]);
  });
});

describe("rows", () => {
  it("total a play over the shown defenses only", () => {
    const g = grid();
    const [smash] = playRows(g, all);
    expect(readRow(g, completion, smash!).reading!.value).toBeCloseTo(14 / 20);
    const [onlyC3] = playRows(g, { rows: [0], cols: [1] });
    expect(readRow(g, completion, onlyC3!).reading!.value).toBeCloseTo(8 / 10);
  });

  it("open a play into one row per shown defense", () => {
    const g = grid();
    const defs = defenseRows(g, 0, all);
    expect(defs.map((d) => d.label)).toEqual(["Cover 2", "Cover 3"]);
    expect(defs.map((d) => readRow(g, completion, d).reading!.value)).toEqual([0.6, 0.8]);
  });

  it("show a metric that means nothing for a row as no reading, with its stats still there", () => {
    const g = grid();
    const iz = playRows(g, all)[1]!;
    const r = readRow(g, completion, iz);
    expect(r.reading).toBeNull();
    expect(r.stats).not.toBeNull();
  });

  it("total each kind and everything, each metric over the rows it applies to", () => {
    const g = grid();
    const totals = totalRows(g, all);
    expect(totals.map((t) => t.label)).toEqual(["All passes", "All runs", "All"]);
    const everything = totals[2]!;
    expect(readRow(g, completion, everything).reading!.value).toBeCloseTo(20 / 40); // the passes only
    expect(readRow(g, ypp, everything).reading!.value).toBeCloseTo((60 * 4 + 40 + 20) / 60); // every play
    expect(readRow(g, ypp, totals[1]!).reading!.value).toBeCloseTo(60 / 20);
  });

  it("leave out the per-kind totals when one kind is shown", () => {
    const g = grid();
    expect(totalRows(g, { rows: [0, 2], cols: [0, 1] }).map((t) => t.label)).toEqual(["All"]);
  });

  it("are pending until a cell arrives", () => {
    const g = new ResultGrid(info, {});
    const [smash] = playRows(g, all);
    expect(readRow(g, completion, smash!)).toEqual({ stats: null, reading: null });
  });
});

describe("sorting", () => {
  it("orders by a metric, rows it means nothing for last, and keeps the order without one", () => {
    const g = grid();
    const rows = playRows(g, all);
    expect(sortRows(g, rows, completion, "desc").map((r) => r.label)).toEqual(["Smash", "Mesh", "Inside Zone"]);
    expect(sortRows(g, rows, completion, "asc").map((r) => r.label)).toEqual(["Mesh", "Smash", "Inside Zone"]);
    expect(sortRows(g, rows, completion, null)).toBe(rows);
  });
});

describe("baselines", () => {
  it("center each kind on its average, spanning the widest play row or cell", () => {
    const g = grid();
    const play = baselines(g, completion, all, "average", "play");
    expect([...play.keys()]).toEqual(["pass"]); // completion means nothing for runs
    expect(play.get("pass")!.center).toBeCloseTo(0.5);
    expect(play.get("pass")!.span).toBeCloseTo(0.2); // smash 0.7, mesh 0.3
    expect(baselines(g, completion, all, "average", "cell").get("pass")!.span).toBeCloseTo(0.3); // 0.8 and 0.2
  });

  it("use only what's shown", () => {
    const g = grid();
    const b = baselines(g, completion, { rows: [0, 2], cols: [1] }, "average", "play").get("pass")!;
    expect(b.center).toBeCloseTo(12 / 20);
  });
});

describe("metric chips", () => {
  it("show every metric with nothing remembered, in the metric list's order, dropping unknown keys", () => {
    expect(metricsShown("scrimmage", undefined).length).toBe(presetsFor("scrimmage")[0]!.keys.length);
    expect(metricsShown("scrimmage", ["int", "gone", "ypp"]).map((m) => m.key)).toEqual(["ypp", "int"]);
  });

  it("have presets whose keys are all metrics", () => {
    for (const domain of ["scrimmage", "kicks"] as const) {
      for (const p of presetsFor(domain)) expect(metricsShown(domain, p.keys).length).toBe(p.keys.length);
    }
    expect(presetsFor("kicks").map((p) => p.name)).toEqual(["All", "Kickoffs", "Punts", "Field goals"]);
  });

  it("read kick stats too", () => {
    const kicks: MatrixInfo = { kind: "kicks", runs: 5, seed: 1, rows: [{ id: "p", name: "Punt", type: "punt" }], cols: [{ id: "pr", name: "Return" }] };
    const g = new ResultGrid(kicks, {});
    const k = { kicks: 5, returned: 3, touchdowns: 0, touchbacks: 0, fair_catches: 2, recovered_kicking: 0, made: 0, blocked: 0,
      distance: 200, hang: 20, net_yards: 180, returns: 3, return_yards: 20 } satisfies KickStats;
    g.add({ row: 0, col: 0, stats: k });
    const [punt] = playRows(g, { rows: [0], cols: [0] });
    expect(readRow(g, metricByKey("net", "kicks"), punt!).reading!.value).toBeCloseTo(36);
    expect(readRow(g, metricByKey("made", "kicks"), punt!).reading).toBeNull();
  });

  it("leave a return that doesn't answer the kick out of its opened rows", () => {
    const kicks: MatrixInfo = {
      kind: "kicks", runs: 5, seed: 1, rows: [{ id: "p", name: "Punt", type: "punt" }],
      cols: [{ id: "kr", name: "Kick return", type: "kickoff_return" }, { id: "pr", name: "Punt return", type: "punt_return" }],
    };
    const g = new ResultGrid(kicks, {});
    const none = { kicks: 0, returned: 0, touchdowns: 0, touchbacks: 0, fair_catches: 0, recovered_kicking: 0, made: 0, blocked: 0,
      distance: 0, hang: 0, net_yards: 0, returns: 0, return_yards: 0 } satisfies KickStats;
    g.add({ row: 0, col: 0, stats: none, applies: false });
    g.add({ row: 0, col: 1, stats: { ...none, kicks: 5 } });
    expect(defenseRows(g, 0, { rows: [0], cols: [0, 1] }).map((d) => d.label)).toEqual(["Punt return"]);
  });
});

describe("export", () => {
  it("writes the rows as laid out, a column per shown metric, empty where a metric means nothing", () => {
    const g = grid();
    const rows = [...playRows(g, all).slice(0, 2), ...defenseRows(g, 1, all).slice(0, 1), ...totalRows(g, all).slice(2)];
    expect(tableCsv(g, [completion, ypp], rows)).toBe(
      "Play,Defense,Completion (of attempts),Yards per play\n" +
      "Smash,All shown,0.7000,6.000\n" +
      "Inside Zone,All shown,,3.000\n" +
      "Inside Zone,Cover 2,,4.000\n" +
      "All,,0.5000,5.000\n",
    );
  });
});
