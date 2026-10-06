import { describe, expect, it } from "vitest";
import { nextDir, sortIndices, toCsv } from "./order";
import { metricByKey } from "./results";

describe("sortIndices", () => {
  const v = [3, null, 9, 3, 1];
  it("keeps the order with no direction", () => {
    expect(sortIndices(5, (i) => v[i]!, null)).toEqual([0, 1, 2, 3, 4]);
  });
  it("sorts descending and ascending, nulls last, ties in the original order", () => {
    expect(sortIndices(5, (i) => v[i] ?? null, "desc")).toEqual([2, 0, 3, 4, 1]);
    expect(sortIndices(5, (i) => v[i] ?? null, "asc")).toEqual([4, 0, 3, 2, 1]);
  });
});

describe("nextDir", () => {
  it("cycles none, descending, ascending, none", () => {
    expect(nextDir(null)).toBe("desc");
    expect(nextDir("desc")).toBe("asc");
    expect(nextDir("asc")).toBeNull();
  });
});

describe("toCsv", () => {
  const rows = [{ name: "Smash" }, { name: 'Mesh, "deep"' }];
  const cols = [{ name: "Cover 2" }, { name: "Cover 3" }];
  // value = (r + 2) / 10 + (c + 2) / 100, where -1 is a total; row 1, column 1 hasn't arrived.
  const read = (r: number, c: number) => (r === 1 && c === 1 ? null : { value: (r + 2) / 10 + (c + 2) / 100, se: null });
  it("writes the shown rows and columns in the shown order with totals, quoting names", () => {
    const csv = toCsv(metricByKey("completion"), rows, cols, read, { rows: [1, 0], cols: [1, 0] });
    expect(csv.split("\n")).toEqual([
      ",Cover 3,Cover 2,All",
      '"Mesh, ""deep""",,0.3200,0.3100',
      "Smash,0.2300,0.2200,0.2100",
      "All,0.1300,0.1200,0.1100",
      "",
    ]);
  });
  it("leaves out hidden rows and columns", () => {
    const csv = toCsv(metricByKey("completion"), rows, cols, read, { rows: [0], cols: [0] });
    expect(csv.split("\n")[0]).toBe(",Cover 2,All");
    expect(csv.split("\n")).toHaveLength(4);
  });
});
