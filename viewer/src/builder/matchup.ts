// The Matchup tab's logic, free of the DOM so it can be tested: the form, how
// many plays a choice adds up to, the play lists, and the server message.

import type { PlayInfo } from "../plays";

export type RunMode = "per_cell" | "total";
export type HashChoice = "rotate" | "left" | "middle" | "right";

export interface MatchupForm {
  offense: string; // team id
  defense: string; // team id; "" = an empty field
  plays: string[]; // offensive play ids
  defs: string[]; // defensive play ids
  mode: RunMode;
  value: number; // runs per cell, or total runs, by mode
  seed: number;
  hash: HashChoice;
  yardLine: number;
  huddle: boolean;
}

export const DEFAULT_FORM: MatchupForm = {
  offense: "",
  defense: "",
  plays: [],
  defs: [],
  mode: "per_cell",
  value: 100,
  seed: 1,
  hash: "rotate",
  yardLine: 25,
  huddle: false,
};

// What a choice of plays and runs adds up to.
export interface RunPlan {
  cells: number;
  perCell: number; // seeds per cell (at least 1)
  total: number; // plays in all (perCell × cells; under "total", rounded down to fit)
}

export function planRuns(rows: number, cols: number, mode: RunMode, value: number): RunPlan {
  const cells = Math.max(0, rows) * Math.max(0, cols);
  const v = Number.isFinite(value) ? Math.max(1, Math.floor(value)) : 1;
  const perCell = mode === "per_cell" ? v : Math.max(1, Math.floor(v / Math.max(1, cells)));
  return { cells, perCell, total: perCell * cells };
}

// "about 40 s", "about 3 min", from a count of plays and the plays per second
// measured on the last run (null: no measurement yet).
export function estimate(total: number, playsPerSecond: number | null): string | null {
  if (!playsPerSecond || playsPerSecond <= 0 || total <= 0) return null;
  const s = total / playsPerSecond;
  if (s < 1) return "under a second";
  if (s < 90) return `about ${Math.round(s)} s`;
  return `about ${Math.round(s / 60)} min`;
}

// The run_matrix message. `plays` and `defs` are always sent explicitly (the
// server would take an empty list to mean "all").
export function matrixRequest(f: MatchupForm, plan: RunPlan): Record<string, unknown> {
  const msg: Record<string, unknown> = {
    type: "run_matrix",
    offense: f.offense,
    defense: f.defense,
    plays: f.plays,
    defs: f.defs,
    runs: plan.perCell,
    seed: f.seed,
    yard_line: f.yardLine,
    huddle: f.huddle,
  };
  if (f.hash !== "rotate") msg.hash = f.hash;
  return msg;
}

// Why the form can't run yet, or null.
export function problem(f: MatchupForm, known: { offense: Set<string>; defs: Set<string> }): string | null {
  if (!f.offense) return "Pick an offense";
  if (!f.plays.some((p) => known.offense.has(p))) return "Check at least one offensive play";
  if (f.defense && !f.defs.some((p) => known.defs.has(p))) return "Check at least one defensive play (or pick the empty field)";
  return null;
}

export interface PlayGroup {
  formation: string;
  name: string;
  plays: PlayInfo[];
}

// Plays grouped by formation, in the playbook's order.
export function groupByFormation(plays: PlayInfo[]): PlayGroup[] {
  const groups: PlayGroup[] = [];
  for (const p of plays) {
    let g = groups.find((x) => x.formation === p.formation);
    if (!g) groups.push((g = { formation: p.formation, name: p.formation_name ?? p.formation, plays: [] }));
    g.plays.push(p);
  }
  return groups;
}

// The ids to check for a quick pick: "all", "none", "pass", "run", or a
// formation's id (adds to / removes from the current choice).
export type QuickPick =
  | { kind: "all" }
  | { kind: "none" }
  | { kind: "type"; type: string }
  | { kind: "formation"; formation: string; on: boolean };

export function quickPick(plays: PlayInfo[], current: string[], pick: QuickPick): string[] {
  if (pick.kind === "all") return plays.map((p) => p.id);
  if (pick.kind === "none") return [];
  if (pick.kind === "type") return plays.filter((p) => p.type === pick.type).map((p) => p.id);
  const ids = new Set(current);
  for (const p of plays) if (p.formation === pick.formation) pick.on ? ids.add(p.id) : ids.delete(p.id);
  return plays.filter((p) => ids.has(p.id)).map((p) => p.id); // keeps the playbook's order
}

// A saved form, with anything missing or wrong taken from the defaults.
export function parseForm(raw: unknown): MatchupForm {
  const r = (raw && typeof raw === "object" ? raw : {}) as Record<string, any>;
  const num = (v: unknown, fallback: number) => (typeof v === "number" && Number.isFinite(v) ? v : fallback);
  const ids = (v: unknown) => (Array.isArray(v) ? v.filter((x): x is string => typeof x === "string") : []);
  return {
    offense: typeof r.offense === "string" ? r.offense : DEFAULT_FORM.offense,
    defense: typeof r.defense === "string" ? r.defense : DEFAULT_FORM.defense,
    plays: ids(r.plays),
    defs: ids(r.defs),
    mode: r.mode === "total" || r.mode === "per_cell" ? r.mode : DEFAULT_FORM.mode,
    value: Math.max(1, Math.floor(num(r.value, DEFAULT_FORM.value))),
    seed: Math.max(0, Math.floor(num(r.seed, DEFAULT_FORM.seed))),
    hash: ["rotate", "left", "middle", "right"].includes(r.hash) ? r.hash : DEFAULT_FORM.hash,
    yardLine: Math.min(99, Math.max(1, num(r.yardLine, DEFAULT_FORM.yardLine))),
    huddle: typeof r.huddle === "boolean" ? r.huddle : DEFAULT_FORM.huddle,
  };
}
