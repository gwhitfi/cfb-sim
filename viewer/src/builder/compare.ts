// Comparing two runs: a saved one (A) beside the run on screen (B). Plays and
// defenses are matched by id; every total counts only what both runs have, so
// the two sides total the same matchups. Each metric's Δ is B − A, and a Δ
// smaller than the 95% interval of the difference may be luck. Pure, so it's tested.

import { csvField, sortIndices, type SortDir } from "./order";
import { domainOf, formatValue, settingsText, type Metric, type Reading, type ResultGrid } from "./results";
import { readRow, type Shown, type TableRow } from "./table";

export interface Comparison {
  a: ResultGrid;
  b: ResultGrid;
  rowInA: Map<number, number>; // B's row index -> A's
  colInA: Map<number, number>; // B's column index -> A's
  onlyA: { plays: string[]; defs: string[] }; // names
  onlyB: { plays: string[]; defs: string[] };
}

// Why two runs can't be compared, or null.
export function cannotCompare(a: ResultGrid, b: ResultGrid): string | null {
  if (domainOf(a.info) !== domainOf(b.info)) return "A play run and a kick run can't be compared.";
  return null;
}

function match<T extends { id: string; name: string }>(a: T[], b: T[]) {
  const inA = new Map<number, number>();
  const ids = new Map(a.map((p, i) => [p.id, i]));
  b.forEach((p, i) => {
    const j = ids.get(p.id);
    if (j !== undefined) inA.set(i, j);
  });
  const inB = new Set(b.map((p) => p.id));
  return {
    inA,
    onlyA: a.filter((p) => !inB.has(p.id)).map((p) => p.name),
    onlyB: b.filter((p) => !ids.has(p.id)).map((p) => p.name),
  };
}

export function compare(a: ResultGrid, b: ResultGrid): Comparison {
  const rows = match(a.info.rows, b.info.rows);
  const cols = match(a.info.cols, b.info.cols);
  return {
    a, b, rowInA: rows.inA, colInA: cols.inA,
    onlyA: { plays: rows.onlyA, defs: cols.onlyA },
    onlyB: { plays: rows.onlyB, defs: cols.onlyB },
  };
}

// What's shown of B that A has too.
export function sharedShown(cmp: Comparison, vis: Shown): Shown {
  return { rows: vis.rows.filter((r) => cmp.rowInA.has(r)), cols: vis.cols.filter((c) => cmp.colInA.has(c)) };
}

// A row of B's table as the same row of A's.
export function inA(cmp: Comparison, row: TableRow): TableRow {
  const rows = row.rows.flatMap((r) => (cmp.rowInA.has(r) ? [cmp.rowInA.get(r)!] : []));
  const cols = row.cols.flatMap((c) => (cmp.colInA.has(c) ? [cmp.colInA.get(c)!] : []));
  return {
    ...row, rows, cols,
    ...(row.row !== undefined ? { row: cmp.rowInA.get(row.row) } : {}),
    ...(row.col !== undefined ? { col: cmp.colInA.get(row.col) } : {}),
  };
}

export interface Delta {
  a: Reading | null;
  b: Reading | null;
  d: number | null; // B − A
  noise: number | null; // the 95% interval of the difference (null: a spread isn't known)
  withinNoise: boolean; // |d| is smaller than it: might be luck
}

export function delta(a: Reading | null, b: Reading | null): Delta {
  if (!a || !b) return { a, b, d: null, noise: null, withinNoise: false };
  const d = b.value - a.value;
  const noise = a.se !== null && b.se !== null ? 1.96 * Math.sqrt(a.se * a.se + b.se * b.se) : null;
  return { a, b, d, noise, withinNoise: noise !== null && Math.abs(d) < noise };
}

// A metric's A, B, and Δ for a row of B's table (B's indices).
export function readDelta(cmp: Comparison, m: Metric, row: TableRow): Delta {
  return delta(readRow(cmp.a, m, inA(cmp, row)).reading, readRow(cmp.b, m, row).reading);
}

// How strongly a Δ favors a side, -1 to 1, positive for the offense (or the
// kicking team). `span`: the Δ that is the full color.
export function deltaAdvantage(m: Metric, d: number, span: number): number {
  return Math.max(-1, Math.min(1, d / (span > 1e-9 ? span : 1))) * m.offenseGood;
}

// The widest Δ among these: a column's full color.
export function deltaSpan(deltas: Delta[]): number {
  return deltas.reduce((w, x) => (x.d === null ? w : Math.max(w, Math.abs(x.d))), 0);
}

// Rows ordered by a metric's A, B, or Δ (nulls last); unsorted without one.
export type CompareSide = "a" | "b" | "d";
export function sortCompared(cmp: Comparison, rows: TableRow[], m: Metric | null, side: CompareSide, dir: SortDir | null): TableRow[] {
  if (!m || !dir) return rows;
  const value = (row: TableRow) => {
    const x = readDelta(cmp, m, row);
    return side === "d" ? x.d : (side === "a" ? x.a : x.b)?.value ?? null;
  };
  return sortIndices(rows.length, (i) => value(rows[i]!), dir).map((i) => rows[i]!);
}

// ---- settings

export interface SettingDiff {
  label: string;
  a: string;
  b: string;
  sameTest: boolean; // false: the runs didn't test the same thing (a different spot, hash, huddle, or teams)
}

// The settings that differ between the runs. More runs or another seed is the
// same test; another yard line, hash, huddle, return aggression, or team isn't.
export function settingsDiff(a: ResultGrid, b: ResultGrid): SettingDiff[] {
  const kicks = domainOf(b.info) === "kicks";
  // The settings line's parts, by what they are: "40 runs per cell" -> runs.
  const parts = (g: ResultGrid) => settingsText(g.info, g.settings).split(" · ");
  const labels = kicks ? ["runs", "seed", "hash", "spot", "return aggression"] : ["runs", "seed", "hash", "yard line", "huddle"];
  const pa = parts(a), pb = parts(b);
  const out: SettingDiff[] = [];
  labels.forEach((label, i) => {
    if (pa[i] !== pb[i]) out.push({ label, a: pa[i] ?? "", b: pb[i] ?? "", sameTest: label === "runs" || label === "seed" });
  });
  const teams = kicks ? (["kicking", "receiving"] as const) : (["offense", "defense"] as const);
  for (const t of teams) {
    if ((a.info[t] ?? "") !== (b.info[t] ?? "")) out.push({ label: t, a: a.info[t] || "(none)", b: b.info[t] || "(none)", sameTest: false });
  }
  return out;
}

// ---- export

// The compared table as CSV: A, B, and Δ for each shown metric. Rates are fractions.
export function compareCsv(cmp: Comparison, metrics: Metric[], rows: TableRow[]): string {
  const g = cmp.b;
  const kicks = g.domain === "kicks";
  const num = (m: Metric, v: number | null | undefined) =>
    v === null || v === undefined ? "" : formatValue({ ...m, percent: false, digits: m.percent ? 4 : 3 }, v);
  const lines = [[kicks ? "Kick" : "Play", kicks ? "Return" : "Defense", ...metrics.flatMap((m) => [`${m.label} A`, `${m.label} B`, `${m.label} Δ`])]];
  for (const row of rows) {
    const where =
      row.level === "play" ? [row.label, "All shown"]
      : row.level === "def" ? [g.info.rows[row.row!]!.name, row.label]
      : [row.label, ""];
    lines.push([...where, ...metrics.flatMap((m) => {
      const x = readDelta(cmp, m, row);
      return [num(m, x.a?.value), num(m, x.b?.value), num(m, x.d)];
    })]);
  }
  return lines.map((l) => l.map(csvField).join(",")).join("\n") + "\n";
}
