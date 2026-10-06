import { describe, expect, it } from "vitest";
import type { PlayInfo } from "../plays";
import { DEFAULT_FORM, countText, estimate, groupByFormation, matrixRequest, parseForm, planRuns, problem, quickPick, teamLabel } from "./matchup";

const plays: PlayInfo[] = [
  { id: "smash", name: "Smash", type: "pass", formation: "gun_2x2", formation_name: "Gun 2x2" },
  { id: "mesh", name: "Mesh", type: "pass", formation: "gun_2x2", formation_name: "Gun 2x2" },
  { id: "iz", name: "Inside Zone", type: "run", formation: "gun_trips", formation_name: "Gun Trips" },
];

describe("planRuns", () => {
  it("per cell: runs times cells", () => {
    expect(planRuns(3, 4, "per_cell", 50)).toEqual({ cells: 12, perCell: 50, total: 600 });
  });
  it("total: splits evenly and rounds down, so the total can come up short", () => {
    expect(planRuns(3, 4, "total", 1000)).toEqual({ cells: 12, perCell: 83, total: 996 });
  });
  it("never goes below one run per cell", () => {
    expect(planRuns(10, 10, "total", 5)).toEqual({ cells: 100, perCell: 1, total: 100 });
  });
  it("handles an empty choice and junk values", () => {
    expect(planRuns(0, 5, "per_cell", 10).total).toBe(0);
    expect(planRuns(2, 2, "per_cell", Number.NaN).perCell).toBe(1);
    expect(planRuns(2, 2, "per_cell", 7.9).perCell).toBe(7);
  });
});

describe("estimate", () => {
  it("is blank until a run has measured the speed", () => {
    expect(estimate(1000, null)).toBeNull();
  });
  it("says seconds, then minutes", () => {
    expect(estimate(4000, 100)).toBe("about 40 s");
    expect(estimate(60000, 100)).toBe("about 10 min");
    expect(estimate(10, 1000)).toBe("under a second");
  });
});

describe("the server message", () => {
  const f = { ...DEFAULT_FORM, offense: "a", defense: "b", plays: ["smash"], defs: ["c2"], seed: 4, yardLine: 30 };
  it("sends the plan's runs per cell and the explicit play lists", () => {
    expect(matrixRequest(f, planRuns(1, 1, "per_cell", 25))).toEqual({
      type: "run_matrix", offense: "a", defense: "b", plays: ["smash"], defs: ["c2"], runs: 25, seed: 4, yard_line: 30, huddle: false,
    });
  });
  it("leaves the hash out when it rotates, and sends a fixed one", () => {
    expect(matrixRequest(f, planRuns(1, 1, "per_cell", 1))).not.toHaveProperty("hash");
    expect(matrixRequest({ ...f, hash: "left" }, planRuns(1, 1, "per_cell", 1))).toHaveProperty("hash", "left");
  });
});

describe("problem", () => {
  const known = { offense: new Set(["smash"]), defs: new Set(["c2"]) };
  const ok = { ...DEFAULT_FORM, offense: "a", defense: "b", plays: ["smash"], defs: ["c2"] };
  it("accepts a full form, and an empty field with no defensive plays", () => {
    expect(problem(ok, known)).toBeNull();
    expect(problem({ ...ok, defense: "", defs: [] }, known)).toBeNull();
  });
  it("names what's missing", () => {
    expect(problem({ ...ok, offense: "" }, known)).toMatch(/offense/);
    expect(problem({ ...ok, plays: [] }, known)).toMatch(/offensive/);
    expect(problem({ ...ok, defs: [] }, known)).toMatch(/defensive/);
    expect(problem({ ...ok, plays: ["gone"] }, known)).toMatch(/offensive/); // not in the playbook any more
  });
});

describe("play lists", () => {
  it("groups by formation in playbook order", () => {
    const g = groupByFormation(plays);
    expect(g.map((x) => [x.name, x.plays.length])).toEqual([["Gun 2x2", 2], ["Gun Trips", 1]]);
  });
  it("quick picks", () => {
    expect(quickPick(plays, [], { kind: "all" })).toEqual(["smash", "mesh", "iz"]);
    expect(quickPick(plays, ["smash"], { kind: "none" })).toEqual([]);
    expect(quickPick(plays, [], { kind: "type", type: "run" })).toEqual(["iz"]);
    expect(quickPick(plays, ["iz"], { kind: "formation", formation: "gun_2x2", on: true })).toEqual(["smash", "mesh", "iz"]);
    expect(quickPick(plays, ["smash", "mesh"], { kind: "formation", formation: "gun_2x2", on: false })).toEqual([]);
  });
});

describe("parseForm", () => {
  it("falls back to the defaults for anything missing or wrong", () => {
    expect(parseForm(null)).toEqual(DEFAULT_FORM);
    expect(parseForm({ mode: "nope", value: -5, hash: 3, plays: [1, "smash"], yardLine: 500 })).toEqual({
      ...DEFAULT_FORM, value: 1, plays: ["smash"], yardLine: 99,
    });
  });
  it("keeps good values", () => {
    const f = { ...DEFAULT_FORM, offense: "a", defense: "", mode: "total" as const, value: 5000, hash: "right" as const };
    expect(parseForm(JSON.parse(JSON.stringify(f)))).toEqual(f);
  });
});

describe("the sides", () => {
  it("label a team by its name, adding the id only when two share a name", () => {
    const teams = [{ id: "clemson", name: "Clemson" }, { id: "fsu", name: "Florida State" }, { id: "fsu_copy", name: "Florida State" }];
    expect(teamLabel(teams, teams[0]!)).toBe("Clemson");
    expect(teamLabel(teams, teams[2]!)).toBe("Florida State (fsu_copy)");
  });
  it("count what's checked", () => {
    expect(countText(12, 14, "plays")).toBe("12 of 14 plays");
  });
});
