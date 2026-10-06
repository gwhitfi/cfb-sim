import { describe, expect, it } from "vitest";
import { describeSaved, whenOf, type SavedRun } from "./saved";

const run: SavedRun = { file: "out/matrix/20261005_195723_clemson_vs_clemson_2018.json", offense: "clemson", defense: "clemson_2018", runs: 100, rows: 3, cols: 4, cells: 12, cancelled: false };

describe("saved runs", () => {
  it("reads the time from the file name", () => {
    expect(whenOf(run.file)).toBe("2026-10-05 19:57");
    expect(whenOf("out/matrix/odd.json")).toBe("odd.json");
  });
  it("marks a kick matrix", () => {
    expect(describeSaved({ ...run, kind: "kicks", offense: "a", defense: "b", rows: 5, cols: 3 })).toBe("2026-10-05 19:57 · kicks · a kicking vs b · 5×3, 100 runs");
  });
  it("describes a run for a menu", () => {
    expect(describeSaved(run)).toBe("2026-10-05 19:57 · clemson vs clemson_2018 · 3×4, 100 runs");
    expect(describeSaved({ ...run, defense: "", cancelled: true, cells: 5 })).toBe(
      "2026-10-05 19:57 · clemson vs empty field · 3×4, 100 runs (cancelled at 5/12)",
    );
  });
});
