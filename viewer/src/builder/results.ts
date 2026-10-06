// The Results tab's data, free of the DOM: a matrix filling in as the server's
// cells arrive, the totals of any part of it, and the metrics read from each
// cell's stats (a value and how sure it is).

// A cell's stats as the server sends them (PlayStats as JSON). Only the
// numbers the metrics read are named here; the rest ride along.
export interface CellStats {
  plays: number;
  complete: number;
  picks: number;
  sacks: number;
  scrambles: number;
  success: number;
  attempts?: number; // passes thrown, throwaways included (missing in runs saved before it was counted)
  pressured: number;
  fumbles_lost: number;
  gaining: number; // plays that gained (not incompletions, picks, lost fumbles)
  gained_yards: number;
  gained_yards_sq: number;
  completed_yards: number;
  yac: number;
  nonpositive: number;
  ten_plus: number;
  twenty_plus: number;
  [key: string]: unknown;
}

export interface Sample {
  kind: "best" | "median" | "worst";
  run: number;
  seed: number;
  hash: string;
  flip: boolean;
  yards: number;
}

export interface Target {
  metric: string;
  kind: string;
  value: number;
  span: number;
  note: string;
}

// A kick matrix's cell stats (KickStats as JSON), from the kicking team's side.
export interface KickStats {
  kicks: number;
  returned: number;
  touchdowns: number;
  touchbacks: number;
  fair_catches: number;
  recovered_kicking: number;
  made: number;
  blocked: number;
  distance: number;
  hang: number;
  net_yards: number;
  returns: number;
  return_yards: number;
  [key: string]: unknown;
}

export type Stats = CellStats | KickStats;

export type Domain = "scrimmage" | "kicks";

export interface MatrixInfo {
  kind?: Domain; // missing: scrimmage (runs saved before kicks existed)
  offense?: string; // scrimmage
  defense?: string;
  kicking?: string; // kicks
  receiving?: string;
  runs: number;
  seed: number;
  rows: { id: string; name: string; type: string; formation?: string; formation_name?: string }[];
  cols: { id: string; name: string; type?: string; formation?: string; formation_name?: string }[]; // formation: missing in runs saved before it was sent
  targets?: Target[];
}

// What the run was asked for (the run_matrix message); a Watch replays a play with it.
export interface MatrixSettings {
  kind?: Domain;
  offense?: string;
  defense?: string;
  kicking?: string;
  receiving?: string;
  yard_line?: number;
  huddle?: boolean;
  return_aggression?: number;
  [key: string]: unknown;
}

export interface CellMessage {
  row: number;
  col: number;
  stats: Stats;
  samples?: Sample[];
  applies?: boolean; // kicks: does this answer that kick? (false: nothing ran)
}

// What a row is: a kind of scrimmage play or of kick.
export type PlayKind = "pass" | "run" | "kickoff" | "punt" | "field_goal";

// A row's kind from the server's play type.
export function kindOf(type: string): PlayKind {
  return type === "run" || type === "kickoff" || type === "punt" || type === "field_goal" ? type : "pass";
}

export function domainOf(info: MatrixInfo): Domain {
  return info.kind === "kicks" ? "kicks" : "scrimmage";
}

// "Clemson offense vs Clemson 2018", or the kicking team and its opponent for a
// kick matrix. `name` turns a team id into its name (unknown: the id).
export function titleOf(info: MatrixInfo, name: (id: string) => string = (id) => id): string {
  return domainOf(info) === "kicks"
    ? `${name(info.kicking ?? "")} kicking vs ${name(info.receiving ?? "")} returning`
    : `${name(info.offense ?? "")} offense vs ${info.defense ? name(info.defense) : "an empty field"}`;
}

// The run's settings in a line: "100 runs per cell · seed 1 · rotating hash · yard line 25 · no huddle".
export function settingsText(info: MatrixInfo, s: MatrixSettings): string {
  const kicks = domainOf(info) === "kicks";
  const hash = typeof s.hash === "string" ? `${s.hash} hash` : "rotating hash";
  const parts = [`${info.runs} ${kicks ? "kicks" : "runs"} per cell`, `seed ${info.seed}`, hash];
  if (kicks) {
    parts.push(typeof s.yard_line === "number" ? `yard line ${s.yard_line}` : "the usual spot");
    parts.push(`return aggression ${s.return_aggression ?? 50}`);
  } else {
    parts.push(`yard line ${s.yard_line ?? 25}`, s.huddle ? "huddle" : "no huddle");
  }
  return parts.join(" · ");
}

// The matrix of a run: the cells that have arrived, by row and column.
export class ResultGrid {
  readonly cells: (CellMessage | null)[][];
  done = 0;

  constructor(readonly info: MatrixInfo, readonly settings: MatrixSettings) {
    this.cells = info.rows.map(() => info.cols.map(() => null));
  }

  get total(): number {
    return this.info.rows.length * this.info.cols.length;
  }

  rowKind(r: number): PlayKind {
    return kindOf(this.info.rows[r]?.type ?? "");
  }

  add(m: CellMessage): void {
    const row = this.cells[m.row];
    if (!row || m.col < 0 || m.col >= row.length) return;
    if (row[m.col] === null) this.done++;
    row[m.col] = m;
  }

  get domain(): Domain {
    return domainOf(this.info);
  }

  // The kinds of row there are, in the order they first appear.
  kinds(): PlayKind[] {
    return [...new Set(this.info.rows.map((r) => kindOf(r.type)))];
  }

  stats(r: number, c: number): Stats | null {
    return this.cells[r]?.[c]?.stats ?? null;
  }

  // The totals of the cells in these rows and columns that have arrived.
  total_of(rows: number[], cols: number[]): Stats | null {
    const found: Stats[] = [];
    for (const r of rows) for (const c of cols) {
      const s = this.stats(r, c);
      if (s) found.push(s);
    }
    return sum(found);
  }
}

// Adds up the numeric fields of cells (the counts and sums; spreads and tables
// are left out, since they can't be added). Null for no cells.
export function sum<T extends Stats>(cells: T[]): T | null {
  if (cells.length === 0) return null;
  const out: Record<string, number> = {};
  for (const c of cells) {
    for (const [k, v] of Object.entries(c)) if (typeof v === "number") out[k] = (out[k] ?? 0) + v;
  }
  return out as unknown as T;
}

// How many plays or kicks went into stats.
export function countOf(s: Stats): number {
  const plays = (s as CellStats).plays;
  return typeof plays === "number" ? plays : (s as KickStats).kicks;
}

// ---- metrics

export interface Reading {
  value: number;
  se: number | null; // standard error, where it can be told
}

export interface Metric {
  key: string; // the id the calibration targets use
  label: string;
  kinds: PlayKind[] | "all"; // which rows it means anything for
  digits: number;
  percent: boolean;
  offenseGood: 1 | -1; // is more of it good for the offense / kicking team (1) or the other side (-1)?
  noise?: number; // a 95% interval wider than this (same units) dims the cell
  read(s: Stats): Reading | null;
}

export function appliesTo(m: Metric, kind: PlayKind): boolean {
  return m.kinds === "all" || m.kinds.includes(kind);
}

// Passes thrown, throwaways included. A run saved before the server counted
// them falls back to plays minus sacks and scrambles, which misses a scramble
// that ended in a throw and counts a scramble sacked twice.
export const attempts = (s: CellStats) => (typeof s.attempts === "number" ? s.attempts : s.plays - s.sacks - s.scrambles);

// A share of `den`: the proportion, and its binomial standard error.
function rate(num: number, den: number): Reading | null {
  if (!(den > 0)) return null;
  const p = num / den;
  return { value: p, se: Math.sqrt((p * (1 - p)) / den) };
}

// An average of something we can't tell the spread of.
function average(total: number, n: number): Reading | null {
  return n > 0 ? { value: total / n, se: null } : null;
}

// A scrimmage metric and a kick metric read their own kind of stats.
const scrimmage = (f: (s: CellStats) => Reading | null) => (s: Stats) => f(s as CellStats);
const kick = (f: (s: KickStats) => Reading | null) => (s: Stats) => f(s as KickStats);

export const METRICS: Metric[] = [
  {
    key: "ypp", label: "Yards per play", kinds: "all", digits: 1, percent: false, offenseGood: 1, noise: 1.0,
    read: scrimmage((s) => {
      const n = s.plays;
      if (!(n > 0)) return null;
      const mean = s.gained_yards / n;
      const variance = n > 1 ? Math.max(0, (s.gained_yards_sq / n - mean * mean) * (n / (n - 1))) : 0;
      return { value: mean, se: Math.sqrt(variance / n) };
    }),
  },
  { key: "success", label: "Success (4+ yd)", kinds: "all", digits: 0, percent: true, offenseGood: 1, noise: 0.1, read: scrimmage((s) => rate(s.success, s.plays)) },
  { key: "completion", label: "Completion (of attempts)", kinds: ["pass"], digits: 0, percent: true, offenseGood: 1, noise: 0.1, read: scrimmage((s) => rate(s.complete, attempts(s))) },
  { key: "ypc", label: "Yards per completion", kinds: ["pass"], digits: 1, percent: false, offenseGood: 1, read: scrimmage((s) => average(s.completed_yards, s.complete)) },
  { key: "yac", label: "Yards after catch (per completion)", kinds: ["pass"], digits: 1, percent: false, offenseGood: 1, read: scrimmage((s) => average(s.yac, s.complete)) },
  { key: "int", label: "Interceptions (of attempts)", kinds: ["pass"], digits: 1, percent: true, offenseGood: -1, noise: 0.05, read: scrimmage((s) => rate(s.picks, attempts(s))) },
  { key: "sack", label: "Sacks (of dropbacks)", kinds: ["pass"], digits: 1, percent: true, offenseGood: -1, noise: 0.08, read: scrimmage((s) => rate(s.sacks, s.plays)) },
  { key: "pressure", label: "Pressured", kinds: ["pass"], digits: 0, percent: true, offenseGood: -1, noise: 0.1, read: scrimmage((s) => rate(s.pressured, s.plays)) },
  { key: "fumbles", label: "Fumbles lost", kinds: "all", digits: 1, percent: true, offenseGood: -1, noise: 0.03, read: scrimmage((s) => rate(s.fumbles_lost, s.plays)) },
  { key: "nonpos", label: "Gained 0 or less (of gains)", kinds: "all", digits: 0, percent: true, offenseGood: -1, noise: 0.1, read: scrimmage((s) => rate(s.nonpositive, s.gaining)) },
  { key: "ten", label: "Gained 10+ (of gains)", kinds: "all", digits: 0, percent: true, offenseGood: 1, noise: 0.1, read: scrimmage((s) => rate(s.ten_plus, s.gaining)) },
  { key: "twenty", label: "Gained 20+ (of gains)", kinds: "all", digits: 1, percent: true, offenseGood: 1, noise: 0.06, read: scrimmage((s) => rate(s.twenty_plus, s.gaining)) },
];

// Kick metrics, from the kicking team's side: warm where the kicking team does better.
const KICKS: PlayKind[] = ["kickoff", "punt"];
export const KICK_METRICS: Metric[] = [
  { key: "net", label: "Net yards (kick minus return)", kinds: KICKS, digits: 1, percent: false, offenseGood: 1, read: kick((s) => average(s.net_yards, s.kicks)) },
  { key: "distance", label: "Kick distance", kinds: KICKS, digits: 1, percent: false, offenseGood: 1, read: kick((s) => average(s.distance, s.kicks)) },
  { key: "hang", label: "Hang time (s)", kinds: KICKS, digits: 2, percent: false, offenseGood: 1, read: kick((s) => average(s.hang, s.kicks)) },
  { key: "return", label: "Yards per return", kinds: KICKS, digits: 1, percent: false, offenseGood: -1, read: kick((s) => average(s.return_yards, s.returns)) },
  { key: "returned", label: "Returned (of kicks)", kinds: KICKS, digits: 0, percent: true, offenseGood: -1, noise: 0.1, read: kick((s) => rate(s.returns, s.kicks)) },
  { key: "returntd", label: "Returned for a touchdown", kinds: KICKS, digits: 1, percent: true, offenseGood: -1, noise: 0.05, read: kick((s) => rate(s.touchdowns, s.kicks)) },
  { key: "touchback", label: "Touchbacks (kickoffs)", kinds: ["kickoff"], digits: 0, percent: true, offenseGood: 1, noise: 0.1, read: kick((s) => rate(s.touchbacks, s.kicks)) },
  { key: "faircatch", label: "Fair catches (punts)", kinds: ["punt"], digits: 0, percent: true, offenseGood: 1, noise: 0.1, read: kick((s) => rate(s.fair_catches, s.kicks)) },
  { key: "recovered", label: "Recovered by the kicking team", kinds: ["kickoff"], digits: 1, percent: true, offenseGood: 1, noise: 0.1, read: kick((s) => rate(s.recovered_kicking, s.kicks)) },
  { key: "made", label: "Field goals made", kinds: ["field_goal"], digits: 0, percent: true, offenseGood: 1, noise: 0.1, read: kick((s) => rate(s.made, s.kicks)) },
  { key: "blocked", label: "Field goals blocked", kinds: ["field_goal"], digits: 1, percent: true, offenseGood: -1, noise: 0.05, read: kick((s) => rate(s.blocked, s.kicks)) },
];

export function metricsFor(domain: Domain): Metric[] {
  return domain === "kicks" ? KICK_METRICS : METRICS;
}

export function metricByKey(key: string, domain: Domain = "scrimmage"): Metric {
  const list = metricsFor(domain);
  return list.find((m) => m.key === key) ?? list[0]!;
}

// The kind to read a total of rows with: the metric's one kind, or "both" when it spans several.
export function totalKind(m: Metric): PlayKind | "both" {
  return m.kinds !== "all" && m.kinds.length === 1 ? m.kinds[0]! : "both";
}

// A metric's reading for stats of `kind` rows, or null where it means nothing
// (a pass metric for a run row) or there is nothing to read.
export function reading(m: Metric, s: Stats | null, kind: PlayKind | "both"): Reading | null {
  if (!s) return null;
  if (kind !== "both" && !appliesTo(m, kind)) return null;
  return m.read(s);
}

// 95% interval half-width of a reading (null where the spread isn't known).
export function halfWidth(r: Reading | null): number | null {
  return r && r.se !== null ? 1.96 * r.se : null;
}

// Too few plays to trust the reading?
export function isNoisy(m: Metric, r: Reading | null): boolean {
  const hw = halfWidth(r);
  return m.noise !== undefined && hw !== null && hw > m.noise;
}

export function formatValue(m: Metric, v: number | null): string {
  if (v === null) return "–";
  return m.percent ? `${(v * 100).toFixed(m.digits)}%` : v.toFixed(m.digits);
}

// A difference from the baseline, signed: "+4pt" (percentage points) or "-0.6".
export function formatDelta(m: Metric, d: number | null): string {
  if (d === null) return "–";
  const v = m.percent ? d * 100 : d;
  const text = v.toFixed(m.digits);
  if (Number(text) === 0) return text.replace("-", "");
  return `${v > 0 ? "+" : ""}${text}${m.percent ? "pt" : ""}`;
}
