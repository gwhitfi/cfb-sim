// The defense side of the play builder: a formation, a play (each slot's run
// and pass jobs and optional alignment), the playbook's strength call, what
// each position is offered, the loader's checks, and the JSON written and
// read. No DOM (defense.test.ts).

import { anchorJson, parseAnchor, type Anchor, type Gap, type SideWord } from "./anchors";

export const DEFENSE_POSITIONS = ["DE", "DT", "OLB", "MLB", "CB", "SS", "FS"] as const;
export const PACKAGES = ["base", "nickel", "dime", "goal_line"] as const;
export const COVERAGES = ["cover_0", "cover_1", "cover_2", "cover_3", "cover_4", "fire_zone"] as const;
export const STRENGTH_CALLS = ["te", "passing_strength", "field", "boundary", "back"] as const;
export const SUPPORTS = ["alley_strong", "alley_weak", "force_strong", "force_weak", "deep_third", "deep_fill", "alley", "force"] as const;
export const RUSH_LANES = ["contain", "A", "B", "C", "D"] as const;
export const MAN_TARGETS = ["#1_strong", "#2_strong", "#3_strong", "#1_weak", "#2_weak", "#3_weak", "back_1", "back_2"] as const;
export const DEFENSE_TAGS = [
  "base", "pressure", "man", "zone", "vs_run", "vs_pass", "1st_down", "2nd_long", "3rd_long", "3rd_medium", "3rd_short",
  "short_yardage", "goal_line", "red_zone", "two_minute", "prevent",
] as const;

export interface DefSlot {
  name: string;
  pos: string;
  x: Anchor;
  y: number;
  stance: string; // "" leaves it out of the file
}
export interface DefFormation {
  id: string;
  name: string;
  package: string;
  slots: DefSlot[];
}

export type RunJob = { kind: "gap"; gap: Gap; side: SideWord } | { kind: "support"; ref: string } | { kind: "contain" } | { kind: "pursue" };
export type PassJob =
  | { kind: "rush"; lane: string; side: SideWord | "" }
  | { kind: "zone"; ref: string }
  | { kind: "man"; target: string; leverage: "inside" | "outside" }
  | { kind: "spy" }
  | { kind: "blitz"; gap: Gap; side: SideWord }
  | { kind: "stunt"; lane: string };
export type RunKind = RunJob["kind"];
export type PassKind = PassJob["kind"];

export interface DefAssign {
  run: RunJob;
  pass: PassJob;
  align: { x?: Anchor; y?: number }; // this call's alignment, replacing the formation's
}
export interface DefensePlay {
  id: string;
  name: string;
  coverage: string;
  tags: string[];
  assignments: Record<string, DefAssign>;
}

export type DefRole = "dl" | "lb" | "db";
export function defRoleOf(pos: string): DefRole {
  if (pos === "DE" || pos === "DT") return "dl";
  if (pos === "OLB" || pos === "MLB") return "lb";
  return "db";
}

// What each position is offered, the usual job first.
export const RUN_MENU: Record<DefRole, RunKind[]> = {
  dl: ["gap", "contain", "pursue"],
  lb: ["gap", "support", "contain", "pursue"],
  db: ["support", "gap", "contain", "pursue"],
};
export const PASS_MENU: Record<DefRole, PassKind[]> = {
  dl: ["rush", "zone", "stunt", "spy"],
  lb: ["zone", "man", "blitz", "rush", "spy"],
  db: ["zone", "man", "blitz", "spy"],
};
export const RUN_LABELS: Record<RunKind, string> = { gap: "fill a gap", support: "support", contain: "contain", pursue: "pursue" };
export const PASS_LABELS: Record<PassKind, string> = { rush: "rush", zone: "zone", man: "man", spy: "spy", blitz: "blitz", stunt: "stunt" };

// `side` is strong or weak: the side of the ball he lines up on.
export function makeRun(kind: RunKind, s: DefSlot, side: "strong" | "weak"): RunJob {
  const role = defRoleOf(s.pos);
  switch (kind) {
    case "gap":
      return { kind, gap: role === "dl" ? "B" : "A", side };
    case "support":
      return { kind, ref: s.pos === "CB" ? "deep_third" : s.pos === "FS" ? "deep_fill" : `alley_${side}` };
    case "contain":
      return { kind };
    case "pursue":
      return { kind };
  }
}

export function makePass(kind: PassKind, s: DefSlot, side: "strong" | "weak", zones: readonly string[] = []): PassJob {
  const zone = (want: string) => (zones.length === 0 || zones.includes(want) ? want : zones[0]!);
  switch (kind) {
    case "rush":
      return { kind, lane: "contain", side: "" };
    case "zone":
      return { kind, ref: zone(s.pos === "CB" ? `deep_third_${side}` : s.pos === "FS" ? "deep_middle" : defRoleOf(s.pos) === "dl" ? `curl_flat_${side}` : "hook_middle") };
    case "man":
      return { kind, target: s.pos === "CB" ? `#1_${side}` : `#2_${side}`, leverage: "inside" };
    case "spy":
      return { kind };
    case "blitz":
      return { kind, gap: "A", side };
    case "stunt":
      return { kind, lane: "A" };
  }
}

export function makeAssign(s: DefSlot, side: "strong" | "weak", zones: readonly string[] = []): DefAssign {
  const role = defRoleOf(s.pos);
  return { run: makeRun(RUN_MENU[role][0]!, s, side), pass: makePass(PASS_MENU[role][0]!, s, side, zones), align: {} };
}

// 4-3 Over Cover 3 (data/formations/defense/43_over.json, playbooks/43_base.json).
export function defaultDefenseFormation(): DefFormation {
  const tech = (t: string, side: SideWord): Anchor => ({ kind: "tech", ref: t, side, offset: 0 });
  const d = (name: string, pos: string, x: Anchor, y: number, stance = ""): DefSlot => ({ name, pos, x, y, stance });
  return {
    id: "43_over",
    name: "4-3 Over",
    package: "base",
    slots: [
      d("SDE", "DE", tech("5", "strong"), 1, "three_point"),
      d("DT", "DT", tech("3", "strong"), 1, "three_point"),
      d("NT", "DT", tech("1", "weak"), 1, "three_point"),
      d("WDE", "DE", tech("5", "weak"), 1, "three_point"),
      d("SAM", "OLB", tech("9", "strong"), 4.5),
      d("MIKE", "MLB", { kind: "ball", offset: 0 }, 5),
      d("WILL", "OLB", tech("3", "weak"), 5),
      d("SCB", "CB", { kind: "over", ref: "#1", side: "strong", offset: 1 }, 7),
      d("WCB", "CB", { kind: "over", ref: "#1", side: "weak", offset: 1 }, 7),
      d("SS", "SS", { kind: "hash", side: "strong", offset: 0 }, 9),
      d("FS", "FS", { kind: "ball", offset: 0 }, 13),
    ],
  };
}

export function defaultDefensePlay(): DefensePlay {
  const a = (run: RunJob, pass: PassJob): DefAssign => ({ run, pass, align: {} });
  const gap = (g: Gap, side: SideWord): RunJob => ({ kind: "gap", gap: g, side });
  const rush = (lane: string): PassJob => ({ kind: "rush", lane, side: "" });
  const zone = (ref: string): PassJob => ({ kind: "zone", ref });
  const support = (ref: string): RunJob => ({ kind: "support", ref });
  return {
    id: "43_over_cover3",
    name: "Over Cover 3",
    coverage: "cover_3",
    tags: ["base", "zone", "vs_run", "1st_down"],
    assignments: {
      SDE: a(gap("C", "strong"), rush("contain")),
      DT: a(gap("B", "strong"), rush("B")),
      NT: a(gap("A", "weak"), rush("A")),
      WDE: a(gap("C", "weak"), rush("contain")),
      SAM: a(gap("D", "strong"), zone("curl_flat_strong")),
      MIKE: a(gap("A", "strong"), zone("hook_middle")),
      WILL: a(gap("B", "weak"), zone("curl_flat_weak")),
      SCB: a(support("deep_third"), zone("deep_third_strong")),
      WCB: a(support("deep_third"), zone("deep_third_weak")),
      SS: a(support("alley_strong"), zone("hook_curl_strong")),
      FS: a(support("deep_fill"), zone("deep_middle")),
    },
  };
}

export function newDefSlot(pos: string, taken: string[]): DefSlot {
  const stem: Record<string, string> = { DE: "DE", DT: "DT", OLB: "LB", MLB: "MLB", CB: "NB", SS: "S", FS: "FS" };
  let name = stem[pos] ?? pos;
  for (let i = 2; taken.includes(name); i++) name = `${stem[pos] ?? pos}${i}`;
  const dl = defRoleOf(pos) === "dl";
  return { name, pos, x: { kind: "ball", offset: 0 }, y: dl ? 1 : defRoleOf(pos) === "lb" ? 5 : 8, stance: dl ? "three_point" : "" };
}

// Gives every slot an assignment and drops those for slots that are gone.
export function normalizeDefense(f: DefFormation, p: DefensePlay, sideOf: (name: string) => "strong" | "weak" = () => "strong", zones: readonly string[] = []): void {
  const names = new Set(f.slots.map((s) => s.name));
  for (const k of Object.keys(p.assignments)) if (!names.has(k)) delete p.assignments[k];
  for (const s of f.slots) if (!p.assignments[s.name]) p.assignments[s.name] = makeAssign(s, sideOf(s.name), zones);
}

export function formationProblems(f: DefFormation): string[] {
  const out: string[] = [];
  if (f.slots.length !== 11) out.push(`${f.slots.length} players; a formation has exactly 11`);
  const seen = new Set<string>();
  for (const s of f.slots) {
    if (!s.name.trim()) out.push("a slot has no name");
    else if (seen.has(s.name)) out.push(`two slots are named ${s.name}`);
    seen.add(s.name);
  }
  if (!f.id.trim()) out.push("the formation needs an id");
  return out;
}

export function playProblems(f: DefFormation, p: DefensePlay, calls: string[], zones: readonly string[]): string[] {
  const out: string[] = [];
  const known = (z: string) => zones.length === 0 || zones.includes(z);
  const men = new Map<string, string[]>();
  for (const s of f.slots) {
    const a = p.assignments[s.name];
    if (!a) {
      out.push(`${s.name} has no assignment`);
      continue;
    }
    if (a.pass.kind === "zone" && !known(a.pass.ref)) out.push(`${s.name}: unknown zone '${a.pass.ref}'`);
    if (a.pass.kind === "man") men.set(a.pass.target, [...(men.get(a.pass.target) ?? []), s.name]);
  }
  for (const [target, who] of men) if (who.length > 1) out.push(`${who.join(" and ")} both have ${target} in man`);
  if (calls.length === 0) out.push("the playbook's strength call needs at least one call");
  if (!p.id.trim()) out.push("the play needs an id");
  return out;
}

// ---------------------------------------------------------------------------
// JSON out
// ---------------------------------------------------------------------------

const round = (n: number) => Math.round(n * 100) / 100;

export function formationJson(f: DefFormation): object {
  const slots: Record<string, unknown> = {};
  for (const s of f.slots) slots[s.name] = { pos: s.pos, x: anchorJson(s.x), y: round(s.y), ...(s.stance ? { stance: s.stance } : {}) };
  return { format_version: 1, id: f.id, name: f.name, side: "defense", package: f.package, slots };
}

export function runJson(j: RunJob): object {
  switch (j.kind) {
    case "gap": return { gap: j.gap, side: j.side };
    case "support": return { support: j.ref };
    case "contain": return { contain: true };
    case "pursue": return { pursue: true };
  }
}

export function passJson(j: PassJob): object {
  switch (j.kind) {
    case "rush": return { rush: { lane: j.lane, ...(j.side ? { side: j.side } : {}) } };
    case "zone": return { zone: j.ref };
    case "man": return { man: j.leverage === "inside" ? j.target : { target: j.target, leverage: j.leverage } };
    case "spy": return { spy: true };
    case "blitz": return { blitz: { gap: j.gap, side: j.side } };
    case "stunt": return { stunt: { lane: j.lane } };
  }
}

export function playJson(f: DefFormation, p: DefensePlay): object {
  const assignments: Record<string, unknown> = {};
  for (const s of f.slots) {
    const a = p.assignments[s.name];
    if (!a) continue;
    const align = { ...(a.align.x ? { x: anchorJson(a.align.x) } : {}), ...(a.align.y !== undefined ? { y: round(a.align.y) } : {}) };
    assignments[s.name] = { run: runJson(a.run), pass: passJson(a.pass), ...(Object.keys(align).length ? { align } : {}) };
  }
  return { id: p.id, name: p.name, side: "defense", formation: f.id, ...(p.coverage ? { coverage: p.coverage } : {}), assignments, tags: p.tags };
}

// ---------------------------------------------------------------------------
// JSON in
// ---------------------------------------------------------------------------

type Obj = Record<string, unknown>;
const isObj = (v: unknown): v is Obj => typeof v === "object" && v !== null && !Array.isArray(v);
const str = (v: unknown, fallback = "") => (typeof v === "string" ? v : fallback);
const gapOf = (v: unknown): Gap => (v === "A" || v === "B" || v === "C" || v === "D" ? v : "A");
const sideOf = (v: unknown): SideWord => (typeof v === "string" && ["left", "right", "strong", "weak", "field", "boundary"].includes(v) ? (v as SideWord) : "strong");

export function parseDefenseFormation(j: Obj): DefFormation {
  if (!isObj(j.slots)) throw new Error("a formation needs 'slots'");
  const slots = Object.entries(j.slots).map(([name, v]): DefSlot => {
    if (!isObj(v)) throw new Error(`slot ${name} isn't an object`);
    return { name, pos: str(v.pos, "CB"), x: parseAnchor(v.x ?? 0), y: typeof v.y === "number" ? v.y : 0, stance: str(v.stance) };
  });
  return { id: str(j.id), name: str(j.name), package: str(j.package, "base"), slots };
}

export function parseRun(o: Obj): RunJob {
  if (typeof o.gap === "string") return { kind: "gap", gap: gapOf(o.gap), side: sideOf(o.side) };
  if (typeof o.support === "string") return { kind: "support", ref: o.support };
  if (o.contain) return { kind: "contain" };
  if (o.pursue) return { kind: "pursue" };
  throw new Error(`unknown run job ${JSON.stringify(o)}`);
}

export function parsePass(o: Obj): PassJob {
  if (isObj(o.rush)) return { kind: "rush", lane: str(o.rush.lane) || str(o.rush.gap, "contain"), side: o.rush.side ? sideOf(o.rush.side) : "" };
  if (typeof o.zone === "string") return { kind: "zone", ref: o.zone };
  if (typeof o.man === "string") return { kind: "man", target: o.man, leverage: "inside" };
  if (isObj(o.man)) return { kind: "man", target: str(o.man.target), leverage: o.man.leverage === "outside" ? "outside" : "inside" };
  if (o.spy) return { kind: "spy" };
  if (isObj(o.blitz)) return { kind: "blitz", gap: gapOf(o.blitz.gap), side: sideOf(o.blitz.side) };
  if (isObj(o.stunt)) return { kind: "stunt", lane: str(o.stunt.lane, "A") };
  throw new Error(`unknown pass job ${JSON.stringify(o)}`);
}

export function parseDefensePlay(j: Obj, f: DefFormation): { play: DefensePlay; warnings: string[] } {
  const warnings: string[] = [];
  const formation = str(j.formation);
  if (formation && formation !== f.id) warnings.push(`the play is for formation '${formation}', but the builder has '${f.id}': import that formation first`);
  const p: DefensePlay = { id: str(j.id), name: str(j.name), coverage: str(j.coverage), tags: Array.isArray(j.tags) ? j.tags.filter((t): t is string => typeof t === "string") : [], assignments: {} };
  const names = new Set(f.slots.map((s) => s.name));
  if (isObj(j.assignments)) {
    for (const [name, v] of Object.entries(j.assignments)) {
      if (!names.has(name)) {
        warnings.push(`no slot ${name} in the formation; its assignment was dropped`);
        continue;
      }
      if (!isObj(v) || !isObj(v.run) || !isObj(v.pass)) throw new Error(`${name} needs 'run' and 'pass'`);
      const align: DefAssign["align"] = {};
      if (isObj(v.align)) {
        if (v.align.x !== undefined) align.x = parseAnchor(v.align.x);
        if (typeof v.align.y === "number") align.y = v.align.y;
      }
      p.assignments[name] = { run: parseRun(v.run), pass: parsePass(v.pass), align };
    }
  }
  for (const s of f.slots) if (!p.assignments[s.name]) warnings.push(`${s.name} had no assignment; it was given the usual one`);
  return { play: p, warnings };
}
