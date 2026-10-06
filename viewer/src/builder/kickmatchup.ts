// The kick half of the Matchup tab's logic: which kicks and returns, how many
// cells actually run (a return only answers its own kind of kick), and the
// server message.

import type { PlayInfo } from "../plays";
import { planRuns, type HashChoice, type PlayGroup, type RunMode, type RunPlan } from "./matchup";

export interface KickForm {
  kicking: string; // team ids
  receiving: string;
  kicks: string[]; // kick ids
  returns: string[]; // return and block ids
  mode: RunMode;
  value: number;
  seed: number;
  hash: HashChoice;
  yardLine: number | null; // the kicking team's own yard line; null: each kind's usual spot
  aggression: number; // the receiving team's return aggression, 0-100
}

export const DEFAULT_KICK_FORM: KickForm = {
  kicking: "",
  receiving: "",
  kicks: [],
  returns: [],
  mode: "per_cell",
  value: 100,
  seed: 1,
  hash: "rotate",
  yardLine: null,
  aggression: 50,
};

// What answers each kind of kick.
export const ANSWERS: Record<string, string> = {
  kickoff: "kickoff_return",
  punt: "punt_return",
  field_goal: "field_goal_block",
};

export function answers(kick: PlayInfo, ret: PlayInfo): boolean {
  return ANSWERS[kick.type] === ret.type;
}

// The cells that run: pairs where the return answers the kick.
export function applyingCells(kicks: PlayInfo[], returns: PlayInfo[]): number {
  let n = 0;
  for (const k of kicks) for (const r of returns) if (answers(k, r)) n++;
  return n;
}

// Runs for the chosen kicks and returns; "total" splits over the cells that run.
export function planKickRuns(kicks: PlayInfo[], returns: PlayInfo[], mode: RunMode, value: number): RunPlan {
  const cells = applyingCells(kicks, returns);
  // planRuns counts rows × columns; ask it for one row of `cells` columns.
  return planRuns(1, cells, mode, value);
}

export function kickRequest(f: KickForm, plan: RunPlan): Record<string, unknown> {
  const msg: Record<string, unknown> = {
    type: "run_matrix",
    kind: "kicks",
    kicking: f.kicking,
    receiving: f.receiving,
    kicks: f.kicks,
    returns: f.returns,
    runs: plan.perCell,
    seed: f.seed,
    return_aggression: f.aggression,
  };
  if (f.hash !== "rotate") msg.hash = f.hash;
  if (f.yardLine !== null) msg.yard_line = f.yardLine;
  return msg;
}

// Why the form can't run yet, or null. `kicks` and `returns` are the chosen plays.
export function kickProblem(f: KickForm, kicks: PlayInfo[], returns: PlayInfo[]): string | null {
  if (!f.kicking || !f.receiving) return "Pick the kicking and receiving teams";
  if (kicks.length === 0) return "Check at least one kick";
  if (returns.length === 0) return "Check at least one return or block";
  if (applyingCells(kicks, returns) === 0) return "None of the chosen returns answers the chosen kicks (kickoff returns answer kickoffs, punt returns punts, blocks field goals)";
  return null;
}

export const TYPE_NAMES: Record<string, string> = {
  kickoff: "Kickoffs",
  punt: "Punts",
  field_goal: "Field goals",
  kickoff_return: "Kickoff returns",
  punt_return: "Punt returns",
  field_goal_block: "Field goal blocks",
};

// Kicks or returns grouped by what kind they are, in the playbook's order.
export function groupByType(plays: PlayInfo[]): PlayGroup[] {
  const groups: PlayGroup[] = [];
  for (const p of plays) {
    let g = groups.find((x) => x.formation === p.type);
    if (!g) groups.push((g = { formation: p.type, name: TYPE_NAMES[p.type] ?? p.type, plays: [] }));
    g.plays.push(p);
  }
  return groups;
}

// A saved form, with anything missing or wrong taken from the defaults.
export function parseKickForm(raw: unknown): KickForm {
  const r = (raw && typeof raw === "object" ? raw : {}) as Record<string, any>;
  const num = (v: unknown, fallback: number) => (typeof v === "number" && Number.isFinite(v) ? v : fallback);
  const ids = (v: unknown) => (Array.isArray(v) ? v.filter((x): x is string => typeof x === "string") : []);
  const d = DEFAULT_KICK_FORM;
  return {
    kicking: typeof r.kicking === "string" ? r.kicking : d.kicking,
    receiving: typeof r.receiving === "string" ? r.receiving : d.receiving,
    kicks: ids(r.kicks),
    returns: ids(r.returns),
    mode: r.mode === "total" || r.mode === "per_cell" ? r.mode : d.mode,
    value: Math.max(1, Math.floor(num(r.value, d.value))),
    seed: Math.max(0, Math.floor(num(r.seed, d.seed))),
    hash: ["rotate", "left", "middle", "right"].includes(r.hash) ? r.hash : d.hash,
    yardLine: typeof r.yardLine === "number" && Number.isFinite(r.yardLine) ? Math.min(99, Math.max(1, r.yardLine)) : null,
    aggression: Math.min(100, Math.max(0, num(r.aggression, d.aggression))),
  };
}
