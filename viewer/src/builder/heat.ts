// What a cell is colored against, and the color. The baseline is the metric's
// value that gets no color: the average of the run's own cells, or a
// calibration target. The more a cell differs, the stronger the color: warm
// where the offense is doing better than the baseline, cool where the defense is.

import { reading, type Metric, type PlayKind, type Target } from "./results";

export type BaselineMode = "average" | "targets";

export interface Baseline {
  center: number;
  span: number; // the deviation that is the full color
  source: "average" | "target";
  note?: string; // the target's note
}

// The target for a metric and a kind of rows: a pass or run target beats a "both" one.
export function targetFor(targets: Target[] | undefined, metric: string, kind: PlayKind): Target | undefined {
  return targets?.find((t) => t.metric === metric && t.kind === kind) ?? targets?.find((t) => t.metric === metric && t.kind === "both");
}

// The baseline for cells of `kind`. Under "targets", a metric with no target
// falls back to the run average. `cells` are the values being colored; `average` is
// the metric over all of those cells together (null: nothing yet).
export function baselineFor(
  mode: BaselineMode,
  metric: Metric,
  kind: PlayKind,
  targets: Target[] | undefined,
  cells: number[],
  average: number | null,
): Baseline | null {
  if (mode === "targets") {
    const t = targetFor(targets, metric.key, kind);
    if (t) return { center: t.value, span: t.span, source: "target", note: t.note };
  }
  if (average === null || cells.length === 0) return null;
  const widest = Math.max(...cells.map((v) => Math.abs(v - average)));
  return { center: average, span: widest > 1e-9 ? widest : 1, source: "average" };
}

// How strongly a value stands out, -1 to 1, positive where it favors the offense.
export function advantage(metric: Metric, b: Baseline, value: number): number {
  const dev = Math.max(-1, Math.min(1, (value - b.center) / b.span));
  return dev * metric.offenseGood;
}

const OFFENSE = "255, 176, 32"; // warm
const DEFENSE = "72, 150, 255"; // cool

// A background that works on the dark and light themes: a color over the page.
export function heatColor(adv: number): string {
  const alpha = Math.min(0.62, Math.abs(adv) * 0.62);
  if (alpha < 0.02) return "";
  return `rgba(${adv > 0 ? OFFENSE : DEFENSE}, ${alpha.toFixed(3)})`;
}

// The values of a metric over a set of cells, for the run average's span.
export function valuesOf(metric: Metric, stats: (Parameters<typeof reading>[1])[], kind: PlayKind): number[] {
  const out: number[] = [];
  for (const s of stats) {
    const r = reading(metric, s, kind);
    if (r) out.push(r.value);
  }
  return out;
}
