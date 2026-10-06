// The offense side of the play builder: a formation (its slots), a play (the
// whole-play settings and every slot's assignment), what each position may be
// given, the checks the loader makes (sim/src/data/parse_play.cpp and
// load_dir.cpp's check_play), and the JSON written and read. No DOM, so it is
// unit tested (offense.test.ts).

import { anchorJson, num, parseAnchor, type Anchor, type Side } from "./anchors";

// ---------------------------------------------------------------------------
// Vocabulary (sim/src/data/vocab.h, sim/include/cfbsim/data/enums.h)
// ---------------------------------------------------------------------------

export const OFFENSE_POSITIONS = ["QB", "RB", "FB", "WR", "TE", "OT", "OG", "C"] as const;
export const STANCES = ["snap", "three_point", "two_point", "under_center", "gun", "pistol"] as const;
// A slot in one of these stances is the passer: he needs no assignment.
export const PASSER_STANCES = ["under_center", "gun", "pistol"] as const;
export const BLOCK_TARGETS = [
  "man_on", "man_on_or_inside", "playside_gap", "backside_gap", "first_lb_playside", "emol_playside",
  "man_over", "first_free_rusher", "first_coverage_inside", "kick_out_man",
] as const;
export const READ_TYPES = ["progression", "half_field", "key"] as const;
export const RUN_READS = ["bang", "bend", "bounce"] as const;
export const PATH_TYPES = ["mesh", "aim"] as const;
export const OFFENSE_TAGS = [
  "quick_game", "dropback", "play_action", "screen", "option", "short", "medium", "deep", "inside", "outside", "base",
  "1st_down", "2nd_long", "2nd_short", "3rd_long", "3rd_medium", "3rd_short", "short_yardage", "goal_line", "red_zone",
  "two_minute", "backed_up", "clock_kill", "vs_man", "vs_zone", "vs_cover1", "vs_cover2", "vs_cover3", "vs_cover4", "vs_blitz",
] as const;

// ---------------------------------------------------------------------------
// The formation
// ---------------------------------------------------------------------------

export interface OffSlot {
  name: string;
  pos: string;
  x: Anchor;
  y: number; // yards from the line; behind it is negative
  stance: string; // "" leaves it out of the file
  onLine: boolean; // counts toward the 7 needed on the line
}

export interface OffFormation {
  id: string;
  name: string;
  personnel: string; // "" counts the backs and TEs
  slots: OffSlot[];
}

const slot = (name: string, pos: string, x: Anchor, y: number, stance: string, onLine = false): OffSlot => ({ name, pos, x, y, stance, onLine });

// Gun Wide Trips (data/formations/offense/gun_wide_trips.json) to start from.
export function defaultOffenseFormation(): OffFormation {
  return {
    id: "gun_wide_trips",
    name: "Gun Wide Trips",
    personnel: "11",
    slots: [
      slot("LT", "OT", num(-3), -1, "three_point", true),
      slot("LG", "OG", num(-1.5), -1, "three_point", true),
      slot("C", "C", num(0), -1, "snap", true),
      slot("RG", "OG", num(1.5), -1, "three_point", true),
      slot("RT", "OT", num(3), -1, "three_point", true),
      slot("X", "WR", { kind: "numbers", side: "left", offset: 0 }, -1, "two_point", true),
      slot("Z", "WR", { kind: "numbers", side: "right", offset: 0 }, -1, "two_point", true),
      slot("H", "WR", { kind: "slot", ref: "Z", offset: -5 }, -2, "two_point"),
      slot("Y", "TE", { kind: "slot", ref: "H", offset: -4 }, -2, "two_point"),
      slot("QB", "QB", num(0), -5, "gun"),
      slot("RB", "RB", num(-2), -5, "two_point"),
    ],
  };
}

// A new slot's starting spot and stance for a position.
export function newSlot(pos: string, taken: string[]): OffSlot {
  const base: Record<string, string> = { QB: "QB", RB: "RB", FB: "FB", WR: "W", TE: "U", OT: "T", OG: "G", C: "C" };
  const stem = base[pos] ?? pos;
  let name = stem;
  for (let i = 2; taken.includes(name); i++) name = `${stem}${i}`;
  const line = pos === "OT" || pos === "OG" || pos === "C" || pos === "TE";
  return slot(name, pos, num(pos === "WR" ? 10 : 0), line ? -1 : pos === "QB" ? -5 : pos === "WR" ? -1 : -4, line ? "three_point" : pos === "QB" ? "gun" : "two_point", line);
}

export const personnelOf = (f: OffFormation) => {
  const backs = f.slots.filter((s) => s.pos === "RB" || s.pos === "FB").length;
  const tes = f.slots.filter((s) => s.pos === "TE").length;
  return `${backs}${tes}`;
};

export const isPasser = (s: { stance: string }) => (PASSER_STANCES as readonly string[]).includes(s.stance);

// ---------------------------------------------------------------------------
// Assignments
// ---------------------------------------------------------------------------

export interface RouteRef {
  route: string;
  depth?: number; // moves the route from its first break
  side?: Side; // which sideline is "outside"; unset uses the side he lines up on
}
export interface RouteJob extends RouteRef {
  vsMan?: RouteRef;
  vsZone?: RouteRef;
}
export interface BlockJob {
  action: string;
  target: string; // "" leaves it out
}
// What a slot does: nothing (the QB, the ball carrier), a route, or a block,
// optionally followed by a route ("then"; a back's check release).
export type OffAssign = { kind: "none" } | { kind: "route"; route: RouteJob } | { kind: "block"; block: BlockJob; then?: RouteJob };

export type PlayType = "pass" | "run";
export type Role = "ol" | "qb" | "back" | "wr" | "te";

export function roleOf(pos: string): Role {
  if (pos === "OT" || pos === "OG" || pos === "C") return "ol";
  if (pos === "QB") return "qb";
  if (pos === "RB" || pos === "FB") return "back";
  if (pos === "TE") return "te";
  return "wr";
}

// The kinds of job the editor offers, one level above the JSON.
export type Choice = "none" | "route" | "release" | "protect" | "scheme" | "block";
export const CHOICE_LABELS: Record<Choice, string> = {
  none: "no assignment",
  route: "route",
  release: "check release, then a route",
  protect: "protect (the play's protection)",
  scheme: "block the run scheme",
  block: "a specific block",
};

export function choiceOf(a: OffAssign): Choice {
  if (a.kind === "none") return "none";
  if (a.kind === "route") return "route";
  if (a.then) return "release";
  if (a.block.action === "protect") return "protect";
  if (a.block.action === "scheme") return "scheme";
  return "block";
}

// What each position is offered, the usual job first (from the playbooks).
const MENU: Record<PlayType, Record<Role, Choice[]>> = {
  pass: {
    ol: ["protect", "block"],
    qb: ["none"],
    back: ["release", "route", "protect", "block"],
    wr: ["route", "block"],
    te: ["route", "release", "protect", "block"],
  },
  run: {
    ol: ["scheme", "block"],
    qb: ["none"],
    back: ["block", "route"],
    wr: ["block", "route"],
    te: ["scheme", "block", "route"],
  },
};

// Specific blocks that make sense on each kind of play.
export const BLOCK_ACTIONS: Record<PlayType, readonly string[]> = {
  pass: ["pass_set", "check_release", "stalk"],
  run: ["drive", "reach", "cutoff", "down", "combo_climb", "pull_kick", "pull_lead", "stalk"],
};
// The target each action usually takes (the playbooks' pairs).
export const USUAL_TARGET: Record<string, string> = {
  drive: "emol_playside",
  reach: "man_on",
  cutoff: "backside_gap",
  down: "backside_gap",
  combo_climb: "playside_gap",
  pull_kick: "kick_out_man",
  pull_lead: "first_lb_playside",
  stalk: "man_over",
  pass_set: "man_on_or_inside",
  check_release: "",
};

export const isCarrier = (s: OffSlot, p: OffensePlay) => p.type === "run" && p.run.carrier === s.name;

export function choicesFor(s: OffSlot, p: OffensePlay): Choice[] {
  if (isCarrier(s, p)) return ["none"];
  return MENU[p.type][roleOf(s.pos)];
}

// Would the loader take this assignment for this slot? (protect only on pass
// plays, scheme only on runs, nothing only for the passer and the carrier.)
export function legal(a: OffAssign, s: OffSlot, p: OffensePlay): boolean {
  if (isCarrier(s, p)) return a.kind === "none";
  const c = choiceOf(a);
  if (c === "none") return isPasser(s);
  if (c === "protect") return p.type === "pass";
  if (c === "scheme") return p.type === "run";
  return true;
}

const firstRoute = (routes: readonly string[], want: string) => (routes.includes(want) || routes.length === 0 ? want : routes[0]!);

// A fresh assignment of a kind. `side` is the side he lines up on (a back's
// release goes that way).
export function makeAssign(c: Choice, s: OffSlot, type: PlayType, side: Side, routes: readonly string[] = []): OffAssign {
  const role = roleOf(s.pos);
  switch (c) {
    case "none":
      return { kind: "none" };
    case "route":
      return { kind: "route", route: { route: firstRoute(routes, role === "te" ? "seam" : role === "back" ? "flat" : "go"), ...(role === "back" ? { side } : {}) } };
    case "release":
      return { kind: "block", block: { action: "check_release", target: "" }, then: { route: firstRoute(routes, "flat"), side } };
    case "protect":
      return { kind: "block", block: { action: "protect", target: "" } };
    case "scheme":
      return { kind: "block", block: { action: "scheme", target: "" } };
    case "block": {
      const action = role === "wr" ? "stalk" : type === "pass" ? "pass_set" : "drive";
      // A back leading on a run takes the first linebacker.
      const target = role === "back" && type === "run" ? "first_lb_playside" : USUAL_TARGET[action] ?? "";
      return { kind: "block", block: { action, target } };
    }
  }
}

// ---------------------------------------------------------------------------
// The play
// ---------------------------------------------------------------------------

export interface PassSettings {
  drop: string;
  progression: string[]; // slot names, in read order
  readType: string;
  hot: string; // slot name or ""
  protection: string;
  direction: Side | ""; // the slide's direction
}
export interface PathPoint {
  x: Anchor;
  y: number;
  type: string; // "mesh", "aim", or ""
}
export interface RunSettings {
  scheme: string;
  direction: Side;
  carrier: string;
  then: string; // the QB's fake after the handoff, or ""
  path: PathPoint[];
  freeAfter: string;
  reads: string[];
}
export interface OffensePlay {
  id: string;
  name: string;
  type: PlayType;
  flip: boolean;
  tags: string[];
  pass: PassSettings;
  run: RunSettings;
  // One set per play type, so switching between pass and run loses nothing.
  assignments: Record<PlayType, Record<string, OffAssign>>;
}

export function defaultOffensePlay(): OffensePlay {
  return {
    id: "new_play",
    name: "New Play",
    type: "pass",
    flip: true,
    tags: [],
    pass: { drop: "gun_3", progression: [], readType: "progression", hot: "", protection: "slide", direction: "left" },
    run: {
      scheme: "inside_zone",
      direction: "right",
      carrier: "RB",
      then: "drop_fake",
      path: [
        { x: num(0.5), y: -4.3, type: "mesh" },
        { x: { kind: "gap", ref: "B", side: "right", offset: 0 }, y: -1, type: "aim" },
      ],
      freeAfter: "aim",
      reads: ["bang", "bend", "bounce"],
    },
    assignments: { pass: {}, run: {} },
  };
}

const routeJobs = (a: OffAssign | undefined): RouteJob[] => (!a ? [] : a.kind === "route" ? [a.route] : a.kind === "block" && a.then ? [a.then] : []);
export const runsRoute = (a: OffAssign | undefined) => routeJobs(a).length > 0;
export const currentAssignments = (p: OffensePlay) => p.assignments[p.type];

export function routers(f: OffFormation, p: OffensePlay): string[] {
  const map = currentAssignments(p);
  return f.slots.filter((s) => runsRoute(map[s.name])).map((s) => s.name);
}

// Makes the play fit the formation and the loader: drops assignments for
// slots that are gone, gives every slot a legal assignment for this play type
// (a default where it has none), keeps the carrier a real slot, and keeps the
// progression and hot to slots that run a route.
export function normalizeOffense(f: OffFormation, p: OffensePlay, sideOf: (name: string) => Side = () => "right", routes: readonly string[] = []): void {
  const names = new Set(f.slots.map((s) => s.name));
  for (const map of Object.values(p.assignments)) for (const k of Object.keys(map)) if (!names.has(k)) delete map[k];
  if (!names.has(p.run.carrier)) p.run.carrier = f.slots.find((s) => roleOf(s.pos) === "back")?.name ?? f.slots.find((s) => !["ol", "qb"].includes(roleOf(s.pos)))?.name ?? "";
  const map = currentAssignments(p);
  for (const s of f.slots) {
    const a = map[s.name];
    if (!a || !legal(a, s, p)) map[s.name] = makeAssign(choicesFor(s, p)[0]!, s, p.type, sideOf(s.name), routes);
  }
  const r = new Set(routers(f, p));
  p.pass.progression = [...new Set(p.pass.progression)].filter((n) => r.has(n));
  if (p.pass.hot && !r.has(p.pass.hot)) p.pass.hot = "";
}

// ---------------------------------------------------------------------------
// Checks (what the loader would reject)
// ---------------------------------------------------------------------------

export interface LibNames {
  routes: readonly string[];
  drops: readonly string[];
  fakes: readonly string[];
  protections: readonly string[];
  runSchemes: readonly string[];
}

export function formationProblems(f: OffFormation): string[] {
  const out: string[] = [];
  const s = f.slots;
  if (s.length !== 11) out.push(`${s.length} players; a formation has exactly 11`);
  const seen = new Set<string>();
  for (const x of s) {
    if (!x.name.trim()) out.push("a slot has no name");
    else if (seen.has(x.name)) out.push(`two slots are named ${x.name}`);
    seen.add(x.name);
  }
  const line = s.filter((x) => x.onLine).length;
  if (line < 7) out.push(`${line} on the line; at least 7 are needed`);
  const snappers = s.filter((x) => x.stance === "snap").length;
  if (snappers !== 1) out.push(`${snappers} slots with stance 'snap'; exactly 1 is needed`);
  if (!s.some(isPasser)) out.push("no passer: give the QB the gun, under_center, or pistol stance");
  if (!f.id.trim()) out.push("the formation needs an id");
  return out;
}

export function playProblems(f: OffFormation, p: OffensePlay, lib: LibNames): string[] {
  const out: string[] = [];
  const map = currentAssignments(p);
  const known = (list: readonly string[], v: string) => list.length === 0 || list.includes(v);
  for (const s of f.slots) {
    const a = map[s.name];
    if (!a) out.push(`${s.name} has no assignment`);
    else if (!legal(a, s, p)) {
      const c = choiceOf(a);
      out.push(c === "none" ? `${s.name} has no assignment` : isCarrier(s, p) ? `${s.name} carries the ball; he needs no other job` : `${s.name}: '${c}' isn't allowed on a ${p.type} play`);
    }
    for (const r of routeJobs(a)) {
      for (const ref of [r, r.vsMan, r.vsZone]) if (ref && !known(lib.routes, ref.route)) out.push(`${s.name}: unknown route '${ref.route}'`);
    }
  }
  if (p.type === "pass") {
    const r = new Set(routers(f, p));
    if (p.pass.progression.length === 0) out.push("the progression is empty");
    for (const n of p.pass.progression) if (!r.has(n)) out.push(`progression: ${n} doesn't run a route`);
    if (p.pass.hot && !r.has(p.pass.hot)) out.push(`hot: ${p.pass.hot} doesn't run a route`);
    if (!known(lib.drops, p.pass.drop)) out.push(`unknown drop '${p.pass.drop}'`);
    if (!known(lib.protections, p.pass.protection)) out.push(`unknown protection '${p.pass.protection}'`);
  } else {
    const r = p.run;
    if (!f.slots.some((s) => s.name === r.carrier)) out.push(`the carrier ${r.carrier || "(none)"} isn't in the formation`);
    if (!known(lib.runSchemes, r.scheme)) out.push(`unknown run scheme '${r.scheme}'`);
    if (r.then && !known(lib.fakes, r.then)) out.push(`unknown QB fake '${r.then}'`);
    if (r.path.length === 0) out.push("the run path needs at least one point");
    if (r.freeAfter && !r.path.some((pt) => pt.type === r.freeAfter)) out.push(`free after '${r.freeAfter}', but no path point has that type`);
  }
  if (!p.id.trim()) out.push("the play needs an id");
  return out;
}

// ---------------------------------------------------------------------------
// JSON out
// ---------------------------------------------------------------------------

const round = (n: number) => Math.round(n * 100) / 100;

export function formationJson(f: OffFormation): object {
  const slots: Record<string, unknown> = {};
  for (const s of f.slots) {
    slots[s.name] = { pos: s.pos, x: anchorJson(s.x), y: round(s.y), ...(s.stance ? { stance: s.stance } : {}), ...(s.onLine ? { on_line: true } : {}) };
  }
  return { format_version: 1, id: f.id, name: f.name, side: "offense", personnel: f.personnel || personnelOf(f), slots };
}

const routeRefJson = (r: RouteRef) => ({ route: r.route, ...(r.depth !== undefined ? { depth: r.depth } : {}), ...(r.side ? { side: r.side } : {}) });

function routeJobJson(r: RouteJob): Record<string, unknown> {
  const out: Record<string, unknown> = routeRefJson(r);
  if (r.vsMan || r.vsZone) out.option = { ...(r.vsMan ? { vs_man: routeRefJson(r.vsMan) } : {}), ...(r.vsZone ? { vs_zone: routeRefJson(r.vsZone) } : {}) };
  return out;
}

// One assignment as the playbooks write it; undefined for "none".
export function assignmentJson(a: OffAssign): object | undefined {
  if (a.kind === "none") return undefined;
  if (a.kind === "route") return routeJobJson(a.route);
  return { block: { do: a.block.action, ...(a.block.target ? { target: a.block.target } : {}) }, ...(a.then ? { then: routeJobJson(a.then) } : {}) };
}

export function playJson(f: OffFormation, p: OffensePlay): object {
  const map = currentAssignments(p);
  const assignments: Record<string, unknown> = {};
  for (const s of f.slots) {
    const a = map[s.name] && assignmentJson(map[s.name]!);
    if (a) assignments[s.name] = a;
  }
  const ps = p.pass;
  const r = p.run;
  const body =
    p.type === "pass"
      ? {
          pass: {
            drop: ps.drop,
            progression: ps.progression,
            read_type: ps.readType,
            ...(ps.hot ? { hot: ps.hot } : {}),
            protection: { scheme: ps.protection, ...(ps.direction ? { direction: ps.direction } : {}) },
          },
        }
      : {
          run: {
            scheme: r.scheme,
            direction: r.direction,
            carrier: r.carrier,
            qb_action: { do: "handoff", ...(r.then ? { then: r.then } : {}) },
            path: r.path.map((pt) => ({ x: anchorJson(pt.x), y: round(pt.y), ...(pt.type ? { type: pt.type } : {}) })),
            ...(r.freeAfter ? { free_after: r.freeAfter } : {}),
            reads: r.reads,
          },
        };
  return { id: p.id, name: p.name, side: "offense", type: p.type, formation: f.id, flip: p.flip, ...body, assignments, tags: p.tags };
}

// ---------------------------------------------------------------------------
// JSON in (pasted from data/ or scratch_plays/)
// ---------------------------------------------------------------------------

type Obj = Record<string, unknown>;
const isObj = (v: unknown): v is Obj => typeof v === "object" && v !== null && !Array.isArray(v);
const str = (v: unknown, fallback = "") => (typeof v === "string" ? v : fallback);
const strs = (v: unknown) => (Array.isArray(v) ? v.filter((x): x is string => typeof x === "string") : []);
const side = (v: unknown): Side | undefined => (v === "left" || v === "right" ? v : undefined);

export function parseOffenseFormation(j: Obj): OffFormation {
  if (!isObj(j.slots)) throw new Error("a formation needs 'slots'");
  const slots = Object.entries(j.slots).map(([name, v]): OffSlot => {
    if (!isObj(v)) throw new Error(`slot ${name} isn't an object`);
    return { name, pos: str(v.pos, "WR"), x: parseAnchor(v.x ?? 0), y: typeof v.y === "number" ? v.y : 0, stance: str(v.stance), onLine: v.on_line === true };
  });
  return { id: str(j.id), name: str(j.name), personnel: str(j.personnel), slots };
}

function parseRouteRef(o: Obj): RouteRef {
  return { route: str(o.route), ...(typeof o.depth === "number" ? { depth: o.depth } : {}), ...(side(o.side) ? { side: side(o.side)! } : {}) };
}

function parseRouteJob(o: Obj): RouteJob {
  const r: RouteJob = parseRouteRef(o);
  if (isObj(o.option)) {
    if (isObj(o.option.vs_man)) r.vsMan = parseRouteRef(o.option.vs_man);
    if (isObj(o.option.vs_zone)) r.vsZone = parseRouteRef(o.option.vs_zone);
  }
  return r;
}

// One assignment. Chains longer than "block, then a route" are cut there.
export function parseOffAssign(o: Obj, warn: (w: string) => void): OffAssign {
  if (typeof o.route === "string") {
    if (o.then) warn(`dropped what follows route '${o.route}' (only a block can be followed by a route here)`);
    return { kind: "route", route: parseRouteJob(o) };
  }
  if (isObj(o.block)) {
    const a: OffAssign = { kind: "block", block: { action: str(o.block.do), target: str(o.block.target) } };
    if (isObj(o.then)) {
      if (typeof o.then.route === "string") a.then = parseRouteJob(o.then);
      else warn(`dropped the block after '${a.block.action}' (only a route can follow a block here)`);
    }
    return a;
  }
  throw new Error(`not a route or a block: ${JSON.stringify(o)}`);
}

// A play for the formation `f`. Warnings say what didn't fit.
export function parseOffensePlay(j: Obj, f: OffFormation): { play: OffensePlay; warnings: string[] } {
  const warnings: string[] = [];
  const p = defaultOffensePlay();
  p.id = str(j.id);
  p.name = str(j.name);
  p.type = j.type === "run" ? "run" : "pass";
  p.flip = j.flip === true;
  p.tags = strs(j.tags);
  const formation = str(j.formation);
  if (formation && formation !== f.id) warnings.push(`the play is for formation '${formation}', but the builder has '${f.id}': import that formation first`);
  if (p.type === "pass") {
    if (!isObj(j.pass)) throw new Error("a pass play needs 'pass'");
    const s = j.pass;
    const prot = isObj(s.protection) ? s.protection : {};
    p.pass = { drop: str(s.drop), progression: strs(s.progression), readType: str(s.read_type, "progression"), hot: str(s.hot), protection: str(prot.scheme), direction: side(prot.direction) ?? "" };
  } else {
    if (!isObj(j.run)) throw new Error("a run play needs 'run'");
    const s = j.run;
    const qb = isObj(s.qb_action) ? s.qb_action : {};
    const path = Array.isArray(s.path) ? s.path.filter(isObj) : [];
    p.run = {
      scheme: str(s.scheme),
      direction: side(s.direction) ?? "right",
      carrier: str(s.carrier),
      then: str(qb.then),
      path: path.map((pt) => {
        if (typeof pt.y !== "number") warnings.push("a path point's y isn't a number; it was set to 0");
        return { x: parseAnchor(pt.x ?? 0), y: typeof pt.y === "number" ? pt.y : 0, type: str(pt.type) };
      }),
      freeAfter: str(s.free_after),
      reads: strs(s.reads),
    };
  }
  const names = new Set(f.slots.map((s) => s.name));
  const map = p.assignments[p.type];
  if (isObj(j.assignments)) {
    for (const [name, v] of Object.entries(j.assignments)) {
      if (!names.has(name)) {
        warnings.push(`no slot ${name} in the formation; its assignment was dropped`);
        continue;
      }
      if (isObj(v)) map[name] = parseOffAssign(v, (w) => warnings.push(`${name}: ${w}`));
    }
  }
  for (const s of f.slots) {
    if (map[s.name]) continue;
    if (isPasser(s) || (p.type === "run" && p.run.carrier === s.name)) map[s.name] = { kind: "none" };
    else warnings.push(`${s.name} had no assignment; it was given the usual one`);
  }
  return { play: p, warnings };
}
