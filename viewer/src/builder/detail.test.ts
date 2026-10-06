import { describe, expect, it } from "vitest";
import { detailSections } from "./detail";
import type { CellStats } from "./results";

const stats = {
  plays: 100, complete: 60, picks: 2, sacks: 5, scrambles: 3, success: 50, pressured: 20, fumbles_lost: 1,
  gaining: 90, gained_yards: 600, gained_yards_sq: 9000, completed_yards: 720, yac: 300,
  nonpositive: 9, ten_plus: 20, twenty_plus: 5, sheds: 200, rush_moves: 100, pancakes: 10, holds: 4, hot: 6,
  tackle_tries: 150, broken: 12, missed: 8, evaded: 5, contacted: 50, before_contact: 250, after_contact: 100,
  fumbles: 2,
  gains: { count: 90, mean: 6.7, p10: 0, p50: 5, p90: 18, share_nonpositive: 10, share_10_plus: 22.2, share_20_plus: 5.6 },
  pocket: { p10: 2, p50: 3.5, p90: 5 },
  by_target: { X: { thrown: 40, complete: 25, picks: 1, yards: 300 }, Z: { thrown: 10, complete: 4, picks: 1, yards: 50 } },
  openness: [[0, 0], [10, 2], [20, 10], [15, 12], [5, 5]],
  reads: {},
} as unknown as CellStats;

const find = (sections: ReturnType<typeof detailSections>, title: string) => sections.find((s) => s.title.startsWith(title));

describe("detailSections", () => {
  const pass = detailSections(stats, "pass");
  it("has the headline numbers with their uncertainty", () => {
    const results = find(pass, "Results")!;
    expect(results.title).toBe("Results (100 plays)");
    const completion = results.rows.find(([l]) => l.startsWith("Completion"))!;
    expect(completion[1]).toMatch(/^65% ± \d+%$/); // 60 of 92 attempts
  });
  it("lists the yards spread, pocket, targets, and openness", () => {
    expect(find(pass, "Yards of the 90")!.rows).toContainEqual(["10 / 50 / 90%", "0.0  5.0  18.0"]);
    expect(find(pass, "Protection")!.rows[0]).toEqual(["Pocket held, 10 / 50 / 90%", "2.00  3.50  5.00 s"]);
    expect(find(pass, "Thrown to")!.rows[0]).toEqual(["X", "40 thrown, 63% complete, 1 int, 7.5 yd"]);
    expect(find(pass, "Openness")!.rows).toContainEqual(["0.4–0.6", "10/20 completed (50%)"]);
    expect(find(pass, "Openness")!.rows).toHaveLength(4); // the empty bucket is left out
  });
  it("tackling per play and per contact", () => {
    const t = find(pass, "Tackling")!;
    expect(t.rows[0]![1]).toBe("1.50 / 0.12 / 0.08 / 0.05");
    expect(t.rows[1]![1]).toBe("5.0 / 2.0");
  });
  it("leaves the passing sections off a run", () => {
    const run = detailSections(stats, "run");
    expect(find(run, "Protection")).toBeUndefined();
    expect(find(run, "Results")!.rows.some(([l]) => l.startsWith("Completion"))).toBe(false);
  });
});
