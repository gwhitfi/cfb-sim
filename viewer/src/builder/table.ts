// The Results table, free of the DOM: what the filters leave shown, a row per
// offensive play (totaled over the shown defenses) with a row per defense
// under it, the All rows, sorting by a metric, each column's baseline, and the
// metric chips' presets. Plays and defenses are filtered by id, so a filter
// means the same thing in another run of the same teams.

import { baselineFor, type Baseline, type BaselineMode } from "./heat";
import { TYPE_NAMES } from "./kickmatchup";
import { csvField, sortIndices, type SortDir } from "./order";
import {
  appliesTo, domainOf, formatValue, kindOf, metricsFor, reading, totalKind,
  type Domain, type MatrixInfo, type Metric, type PlayKind, type Reading, type ResultGrid, type Stats,
} from "./results";

// ---- filters

// What's hidden; nothing hidden shows everything.
export interface Filters {
  kinds: PlayKind[]; // play types (pass, run; or kinds of kick)
  plays: string[]; // offensive play (or kick) ids
  defs: string[]; // defensive play (or return) ids
}

export const NO_FILTERS: Filters = { kinds: [], plays: [], defs: [] };

// The grid's row and column indices the filters leave shown, in the run's order.
export interface Shown {
  rows: number[];
  cols: number[];
}

export function shown(info: MatrixInfo, f: Filters): Shown {
  const rows: number[] = [];
  info.rows.forEach((p, i) => {
    if (!f.kinds.includes(kindOf(p.type)) && !f.plays.includes(p.id)) rows.push(i);
  });
  const cols: number[] = [];
  info.cols.forEach((p, i) => {
    if (!f.defs.includes(p.id)) cols.push(i);
  });
  return { rows, cols };
}

export function hiddenCount(f: Filters): number {
  return f.kinds.length + f.plays.length + f.defs.length;
}

// A hidden list with `ids` hidden (on: false) or shown (on: true).
export function setShown(hidden: string[], ids: string[], on: boolean): string[] {
  const out = new Set(hidden);
  for (const id of ids) on ? out.delete(id) : out.add(id);
  return [...out];
}

// A checkbox menu's groups: plays (or defenses) by formation; kicks (or
// returns) by kind. A run saved before formations were sent is one group.
export interface FilterGroup {
  key: string;
  name: string;
  items: { id: string; name: string }[];
}

export function filterGroups(info: MatrixInfo, side: "plays" | "defs"): FilterGroup[] {
  const kicks = domainOf(info) === "kicks";
  const list = side === "plays" ? info.rows : info.cols;
  const groups: FilterGroup[] = [];
  for (const p of list) {
    const key = (kicks ? p.type : p.formation) ?? "";
    const name = kicks ? TYPE_NAMES[key] ?? key : p.formation_name || p.formation || (side === "plays" ? "Plays" : "Defenses");
    let g = groups.find((x) => x.key === key);
    if (!g) groups.push((g = { key, name, items: [] }));
    g.items.push({ id: p.id, name: p.name });
  }
  return groups;
}

// ---- rows

// A row of the table: the stats of some of the grid's rows over some columns.
export interface TableRow {
  key: string; // unique in the table
  label: string;
  level: "play" | "def" | "total";
  kind: PlayKind | "both"; // "both": a total over kinds; each metric reads the rows it applies to
  rows: number[];
  cols: number[];
  row?: number; // play and def rows: the grid row
  col?: number; // def rows: the grid column
}

// One row per shown play, totaled over the shown defenses.
export function playRows(g: ResultGrid, vis: Shown): TableRow[] {
  return vis.rows.map((r) => ({
    key: `play:${r}`, label: g.info.rows[r]!.name, level: "play", kind: g.rowKind(r), rows: [r], cols: vis.cols, row: r,
  }));
}

// The rows under an opened play: one per shown defense, that cell alone. A
// return that doesn't answer a kick (nothing ran) is left out.
export function defenseRows(g: ResultGrid, r: number, vis: Shown): TableRow[] {
  return vis.cols.filter((c) => g.cells[r]?.[c]?.applies !== false).map((c) => ({
    key: `def:${r}:${c}`, label: g.info.cols[c]!.name, level: "def", kind: g.rowKind(r), rows: [r], cols: [c], row: r, col: c,
  }));
}

const KIND_PLURALS: Record<PlayKind, string> = { pass: "passes", run: "runs", kickoff: "kickoffs", punt: "punts", field_goal: "field goals" };

// The rows at the bottom: All of each kind shown (when more than one is), then All.
export function totalRows(g: ResultGrid, vis: Shown): TableRow[] {
  const kinds = [...new Set(vis.rows.map((r) => g.rowKind(r)))];
  const out: TableRow[] = [];
  if (kinds.length > 1) {
    for (const kind of kinds) {
      out.push({ key: `all:${kind}`, label: `All ${KIND_PLURALS[kind]}`, level: "total", kind, rows: vis.rows.filter((r) => g.rowKind(r) === kind), cols: vis.cols });
    }
  }
  out.push({ key: "all", label: "All", level: "total", kind: "both", rows: vis.rows, cols: vis.cols });
  return out;
}

export interface RowReading {
  stats: Stats | null; // everything the row counts (null: nothing has arrived)
  reading: Reading | null; // null: the metric means nothing here, or there's nothing to read
}

// A metric's reading for a row. A total over kinds reads only the rows the metric applies to.
export function readRow(g: ResultGrid, m: Metric, row: TableRow): RowReading {
  const one = row.rows.length === 1 && row.cols.length === 1;
  const stats = one ? g.stats(row.rows[0]!, row.cols[0]!) : g.total_of(row.rows, row.cols);
  if (row.kind !== "both") return { stats, reading: reading(m, stats, row.kind) };
  const rows = row.rows.filter((r) => appliesTo(m, g.rowKind(r)));
  return { stats, reading: reading(m, g.total_of(rows, row.cols), totalKind(m)) };
}

// ---- sorting

export interface SortBy {
  metric: string;
  dir: SortDir;
}

// Rows ordered by a metric (nulls last, ties in the run's order); unsorted without one.
export function sortRows(g: ResultGrid, rows: TableRow[], m: Metric | null, dir: SortDir | null): TableRow[] {
  if (!m || !dir) return rows;
  return sortIndices(rows.length, (i) => readRow(g, m, rows[i]!).reading?.value ?? null, dir).map((i) => rows[i]!);
}

// ---- color

// A column's baseline for each kind of row shown. "play": play rows are
// colored against the other play rows; "cell": a defense row (or a matrix
// cell) against every shown cell of its kind.
export function baselines(g: ResultGrid, m: Metric, vis: Shown, mode: BaselineMode, level: "play" | "cell"): Map<PlayKind, Baseline | null> {
  const out = new Map<PlayKind, Baseline | null>();
  for (const kind of g.kinds()) {
    if (!appliesTo(m, kind)) continue;
    const rows = vis.rows.filter((r) => g.rowKind(r) === kind);
    if (rows.length === 0) continue;
    const values: number[] = [];
    for (const r of rows) {
      if (level === "play") {
        const v = reading(m, g.total_of([r], vis.cols), kind);
        if (v) values.push(v.value);
      } else {
        for (const c of vis.cols) {
          const v = reading(m, g.stats(r, c), kind);
          if (v) values.push(v.value);
        }
      }
    }
    const avg = reading(m, g.total_of(rows, vis.cols), kind)?.value ?? null;
    out.set(kind, baselineFor(mode, m, kind, g.info.targets, values, avg));
  }
  return out;
}

// ---- metric chips

export interface Preset {
  name: string;
  keys: string[];
}

const SCRIMMAGE_PRESETS: Preset[] = [
  { name: "Passing", keys: ["ypp", "success", "completion", "ypc", "yac", "int", "sack", "pressure"] },
  { name: "Running", keys: ["ypp", "success", "fumbles", "nonpos", "ten", "twenty"] },
  { name: "Explosives", keys: ["ypp", "ten", "twenty"] },
  { name: "Turnovers", keys: ["int", "fumbles"] },
];

// All, then the scrimmage presets, or one per kind of kick (its metrics).
export function presetsFor(domain: Domain): Preset[] {
  const all = { name: "All", keys: metricsFor(domain).map((m) => m.key) };
  if (domain === "scrimmage") return [all, ...SCRIMMAGE_PRESETS];
  const kinds: PlayKind[] = ["kickoff", "punt", "field_goal"];
  return [all, ...kinds.map((k) => ({ name: TYPE_NAMES[k]!, keys: metricsFor("kicks").filter((m) => appliesTo(m, k)).map((m) => m.key) }))];
}

// The shown metrics, in the metric list's order. Nothing remembered: all of
// them; a key that's no longer a metric is dropped.
export function metricsShown(domain: Domain, keys: string[] | undefined): Metric[] {
  const list = metricsFor(domain);
  return keys ? list.filter((m) => keys.includes(m.key)) : list;
}

// ---- export

// The table as CSV, laid out as shown: a column per shown metric, a line per
// row (an opened play's defenses under it, the All rows last). Rates are
// fractions; a metric that means nothing for a row is left empty.
export function tableCsv(g: ResultGrid, metrics: Metric[], rows: TableRow[]): string {
  const kicks = g.domain === "kicks";
  const lines = [[kicks ? "Kick" : "Play", kicks ? "Return" : "Defense", ...metrics.map((m) => m.label)]];
  for (const row of rows) {
    const where =
      row.level === "play" ? [row.label, "All shown"]
      : row.level === "def" ? [g.info.rows[row.row!]!.name, row.label]
      : [row.label, ""];
    lines.push([...where, ...metrics.map((m) => {
      const r = readRow(g, m, row).reading;
      return r ? formatValue({ ...m, percent: false, digits: m.percent ? 4 : 3 }, r.value) : "";
    })]);
  }
  return lines.map((l) => l.map(csvField).join(",")).join("\n") + "\n";
}
