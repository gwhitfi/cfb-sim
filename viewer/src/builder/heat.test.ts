import { describe, expect, it } from "vitest";
import { advantage, baselineFor, heatColor, targetFor } from "./heat";
import { metricByKey, type Target } from "./results";

const targets: Target[] = [
  { metric: "ypp", kind: "both", value: 5.8, span: 3, note: "all plays" },
  { metric: "ypp", kind: "run", value: 5.0, span: 3, note: "designed runs" },
  { metric: "completion", kind: "pass", value: 0.61, span: 0.2, note: "" },
];

describe("targetFor", () => {
  it("prefers the kind's own target over a 'both' one", () => {
    expect(targetFor(targets, "ypp", "run")!.value).toBe(5.0);
    expect(targetFor(targets, "ypp", "pass")!.value).toBe(5.8);
  });
  it("is undefined for a metric with none, or a target for the other kind", () => {
    expect(targetFor(targets, "sack", "pass")).toBeUndefined();
    expect(targetFor(targets, "completion", "run")).toBeUndefined();
    expect(targetFor(undefined, "ypp", "run")).toBeUndefined();
  });
});

describe("baselineFor", () => {
  const ypp = metricByKey("ypp");
  it("run average: centered on the average, the widest cell is the full color", () => {
    const b = baselineFor("average", ypp, "pass", targets, [4, 6, 8], 6)!;
    expect(b).toMatchObject({ center: 6, span: 2, source: "average" });
  });
  it("targets: the target and its span", () => {
    const b = baselineFor("targets", ypp, "run", targets, [4, 6], 5)!;
    expect(b).toMatchObject({ center: 5.0, span: 3, source: "target", note: "designed runs" });
  });
  it("targets mode falls back to the run average where there is no target", () => {
    const b = baselineFor("targets", metricByKey("sack"), "pass", targets, [0.05, 0.09], 0.07)!;
    expect(b.source).toBe("average");
  });
  it("has no baseline with nothing to compare, and survives identical cells", () => {
    expect(baselineFor("average", ypp, "pass", targets, [], null)).toBeNull();
    expect(baselineFor("average", ypp, "pass", targets, [5, 5], 5)!.span).toBe(1);
  });
});

describe("advantage and color", () => {
  const b = { center: 5, span: 2, source: "average" as const };
  it("is positive where the offense does better, whichever way the metric points", () => {
    expect(advantage(metricByKey("ypp"), b, 6)).toBeCloseTo(0.5); // more yards: offense
    expect(advantage(metricByKey("sack"), b, 6)).toBeCloseTo(-0.5); // more sacks: defense
    expect(advantage(metricByKey("ypp"), b, 100)).toBe(1); // capped at the full color
  });
  it("colors warm for the offense, cool for the defense, nothing at the baseline", () => {
    expect(heatColor(0.8)).toMatch(/^rgba\(255, 176, 32,/);
    expect(heatColor(-0.8)).toMatch(/^rgba\(72, 150, 255,/);
    expect(heatColor(0)).toBe("");
    expect(heatColor(0.01)).toBe("");
  });
  it("is stronger the further from the baseline", () => {
    const alpha = (c: string) => Number(/, ([\d.]+)\)$/.exec(c)![1]);
    expect(alpha(heatColor(1))).toBeGreaterThan(alpha(heatColor(0.3)));
  });
});
