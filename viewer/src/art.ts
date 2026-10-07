// Play art: the play menu's pictures of formations and plays, as the sim lays
// them out (cfbsim_playbook_art; docs/PROPOSALS.md#the-play-menu). Every
// position is in the play frame in yards: the ball at the origin on the middle
// hash, +x the offense's right, +y downfield. A flipped play is the mirror of
// its picture (every x negated).

import type { TeamKey } from "./game";

export type Pt = [number, number];

export interface ArtPlayer {
  slot: string;
  role: "qb" | "back" | "wr" | "te" | "ol" | "";
  x: number;
  y: number;
}

export interface FormationArt {
  id: string;
  name: string;
  personnel: string;
  players: ArtPlayer[];
}

export interface ArtRoute {
  slot: string;
  points: Pt[]; // from his alignment
  end: "continue" | "stop" | "settle";
  primary: boolean; // the first read
}

export interface PlayArt {
  id: string;
  name: string;
  formation: string; // its players are this formation's
  type: "pass" | "run";
  flip: boolean; // the play may be flipped
  routes: ArtRoute[];
  qb: Pt[]; // the drop or the fake after the handoff, from his alignment; may be empty
  path: Pt[] | null; // run: the carrier's designed path, from his alignment
  pulls: { slot: string; points: Pt[] }[];
}

export interface PlaybookArt {
  unit?: "offense"; // omitted by the original offense art API
  team: string;
  playbook: string;
  formations: FormationArt[];
  plays: PlayArt[];
}

export interface DefensiveArtPlayer {
  slot: string;
  role: "dl" | "lb" | "cb" | "s" | "";
  x: number;
  y: number;
}

export interface DefensiveFormationArt {
  id: string;
  name: string;
  package: string;
  players: DefensiveArtPlayer[];
}

export type ArtZone =
  | { shape: "box"; drop: Pt; width: number; depth: Pt }
  | { shape: "capsule"; drop: Pt; ends: [Pt, Pt]; radius: number };

// Base defensive assignments only: no offensive players or target references.
export type DefensiveArtJob = { slot: string } & (
  | { kind: "zone"; points: Pt[]; area: ArtZone }
  | { kind: "rush" | "blitz" | "stunt"; points: Pt[]; contain: boolean }
  | { kind: "man"; at: Pt; leverage: "inside" | "outside" }
  | { kind: "spy"; at: Pt }
);

export interface DefensivePlayArt {
  id: string;
  name: string;
  formation: string;
  coverage: string;
  players: DefensiveArtPlayer[]; // each call may override the formation's alignment
  jobs: DefensiveArtJob[];
}

export interface DefensivePlaybookArt {
  unit: "defense";
  team: string;
  playbook: string;
  formations: DefensiveFormationArt[];
  plays: DefensivePlayArt[];
}

export type ArtMessage = { side: TeamKey; error: string } & (
  | { unit: "offense"; art: PlaybookArt | null }
  | { unit: "defense"; art: DefensivePlaybookArt | null }
);

// Missing envelope unit means legacy offense. A defensive payload must be
// explicitly marked, so it can never be sent to the offensive renderer.
export function readArtMessage(m: unknown): ArtMessage | null {
  if (!m || typeof m !== "object") return null;
  const o = m as Record<string, unknown>;
  if (o.type !== "playbook_art" || (o.team !== "home" && o.team !== "away")) return null;
  const unit = o.unit === undefined ? "offense" : o.unit;
  if (unit !== "offense" && unit !== "defense") return null;
  const a = o.art as Record<string, unknown> | null | undefined;
  const valid = a && typeof a.team === "string" && typeof a.playbook === "string" &&
    Array.isArray(a.formations) && Array.isArray(a.plays) &&
    (unit === "defense" ? a.unit === "defense" : a.unit === undefined || a.unit === "offense");
  if (typeof o.error === "string" || !valid) {
    return { side: o.team, unit, art: null, error: typeof o.error === "string" ? o.error : `invalid ${unit} playbook art` };
  }
  return unit === "defense"
    ? { side: o.team, unit, art: a as unknown as DefensivePlaybookArt, error: "" }
    : { side: o.team, unit, art: a as unknown as PlaybookArt, error: "" };
}

interface UnitArt {
  offense?: PlaybookArt;
  defense?: DefensivePlaybookArt;
}

// One game, two team sides, two units per side. A failed refresh removes
// only that entry, allowing the caller to fall back to its text menu.
export class PlaybookArtStore {
  home: UnitArt = {};
  away: UnitArt = {};

  clear(): void {
    this.home = {};
    this.away = {};
  }

  receive(m: unknown): ArtMessage | null {
    const r = readArtMessage(m);
    if (!r) return null;
    if (r.unit === "offense") {
      if (r.art) this[r.side].offense = r.art;
      else delete this[r.side].offense;
    } else {
      if (r.art) this[r.side].defense = r.art;
      else delete this[r.side].defense;
    }
    return r;
  }
}

// ---------------------------------------------------------------------------
// Drawing: SVG markup for a card. Pure (data in, a string out) so it runs
// anywhere; the colors are CSS classes (style.css, "Play art").
// ---------------------------------------------------------------------------

// What a card shows, in yards: sideline to sideline, 8 yd behind the line to
// 18 yd past it. SVG y runs down, so a play-frame point (x, y) is drawn at (x, -y).
export const ART_HALF_WIDTH = 160 / 6; // 26.67: the ball to a sideline
export const ART_BACK = 8;
export const ART_DEEP = 18;
const EDGE = 0.6; // yd: a route past the top is cut this far inside it
// Marker sizes (yd): linemen stand 1.5 apart, so theirs are small; the
// lettered skill players are big enough to read on a small card.
const PLAYER_R = { skill: 1.6, qb: 1.1, ol: 0.72 };
const ARROW = 1.3; // yd: an arrowhead's arm
const BAR = 0.9; // yd: half a stop bar

export interface ArtOptions {
  flip?: boolean; // draw the play mirrored (only for a play that may flip)
}

const n = (v: number) => String(Math.round(v * 100) / 100);
const esc = (t: string) => t.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
// A play-frame point as SVG coordinates, mirrored when flipped.
const at = (p: Pt, flip: boolean): Pt => [flip ? -p[0] : p[0], -p[1]];
const pts = (list: Pt[]) => list.map((p) => `${n(p[0])},${n(p[1])}`).join(" ");

// Cuts a play-frame line where it first passes `maxY`: the points up to
// there, and whether it was cut.
export function cutAt(points: Pt[], maxY: number): { points: Pt[]; cut: boolean } {
  const out: Pt[] = [];
  for (let k = 0; k < points.length; k++) {
    const p = points[k]!;
    if (p[1] <= maxY) {
      out.push(p);
      continue;
    }
    const q = out[out.length - 1];
    if (q) {
      const t = (maxY - q[1]) / (p[1] - q[1]);
      out.push([q[0] + t * (p[0] - q[0]), maxY]);
    }
    return { points: out, cut: true };
  }
  return { points: out, cut: false };
}

// The end mark of a line drawn from `a` to `b` (SVG coordinates).
function endMark(a: Pt, b: Pt, end: ArtRoute["end"], cls: string): string {
  const ang = Math.atan2(b[1] - a[1], b[0] - a[0]);
  if (end === "continue") {
    const arm = (d: number): Pt => [b[0] - ARROW * Math.cos(ang + d), b[1] - ARROW * Math.sin(ang + d)];
    return `<polyline class="${cls} art-head" points="${pts([arm(0.5), b, arm(-0.5)])}"/>`;
  }
  if (end === "stop") {
    const d: Pt = [-Math.sin(ang) * BAR, Math.cos(ang) * BAR];
    return `<line class="${cls} art-head" x1="${n(b[0] - d[0])}" y1="${n(b[1] - d[1])}" x2="${n(b[0] + d[0])}" y2="${n(b[1] + d[1])}"/>`;
  }
  return `<circle class="${cls} art-settle" cx="${n(b[0])}" cy="${n(b[1])}" r="0.7"/>`;
}

// A line with its end mark, cut at the top of the card (a cut line ends in an arrow).
function line(points: Pt[], end: ArtRoute["end"], cls: string, flip: boolean): string {
  const cut = cutAt(points, ART_DEEP - EDGE);
  const list = cut.points.map((p) => at(p, flip));
  if (list.length < 2) return "";
  const mark = endMark(list[list.length - 2]!, list[list.length - 1]!, cut.cut ? "continue" : end, cls);
  return `<polyline class="${cls}" points="${pts(list)}"/>${mark}`;
}

function field(): string {
  const w = ART_HALF_WIDTH;
  let out = `<rect class="art-field" x="${n(-w)}" y="${-ART_DEEP}" width="${n(2 * w)}" height="${ART_DEEP + ART_BACK}"/>`;
  for (let y = -5; y < ART_DEEP; y += 5) {
    if (y !== 0) out += `<line class="art-yard" x1="${n(-w)}" y1="${-y}" x2="${n(w)}" y2="${-y}"/>`;
  }
  return out + `<line class="art-los" x1="${n(-w)}" y1="0" x2="${n(w)}" y2="0"/>`;
}

function players(list: ArtPlayer[], flip: boolean): string {
  let out = "";
  for (const p of list) {
    const [x, y] = at([p.x, p.y], flip);
    const role = p.role || "other";
    // Letters on the skill players; linemen and the QB are plain markers.
    const skill = role !== "ol" && role !== "qb";
    const r = skill ? PLAYER_R.skill : role === "ol" ? PLAYER_R.ol : PLAYER_R.qb;
    out += `<circle class="art-player art-${role}" cx="${n(x)}" cy="${n(y)}" r="${r}"/>`;
    if (skill) {
      const size = p.slot.length <= 1 ? 2.1 : 1.5;
      out += `<text class="art-slot" x="${n(x)}" y="${n(y + size * 0.36)}" font-size="${size}" text-anchor="middle">${esc(p.slot)}</text>`;
    }
  }
  return out;
}

function svg(body: string, title: string, defense = false): string {
  const w = ART_HALF_WIDTH;
  return (
    `<svg class="play-art${defense ? " art-defense" : ""}"${defense ? ' overflow="hidden"' : ""} xmlns="http://www.w3.org/2000/svg" viewBox="${n(-w)} ${-ART_DEEP} ${n(2 * w)} ${ART_DEEP + ART_BACK}" ` +
    `role="img" aria-label="${esc(title)}">${body}</svg>`
  );
}

// A formation card's picture: the players, no routes.
export function formationSvg(f: FormationArt, opts: ArtOptions = {}): string {
  return svg(field() + players(f.players, !!opts.flip), f.name);
}

// A play card's picture: its formation's players, the routes (the first
// read's in the primary class), the QB's drop or fake, the run path, and
// pullers. `flip` mirrors it only if the play may flip.
export function playSvg(p: PlayArt, f: FormationArt, opts: ArtOptions = {}): string {
  const flip = !!opts.flip && p.flip;
  let body = field();
  for (const l of p.pulls) body += line(l.points, "continue", "art-pull", flip);
  if (p.qb.length > 1) body += `<polyline class="art-drop" points="${pts(p.qb.map((q) => at(q, flip)))}"/>`;
  for (const r of p.routes) body += line(r.points, r.end, r.primary ? "art-route primary" : "art-route", flip);
  if (p.path) body += line(p.path, "continue", "art-path primary", flip);
  return svg(body + players(f.players, flip), p.name);
}

// ---------------------------------------------------------------------------
// Defensive base cards. The call supplies its own alignment overrides. All
// geometry is already resolved by the sim; no opponents or targets are drawn.
// ---------------------------------------------------------------------------

function defensivePlayers(list: DefensiveArtPlayer[]): string {
  return list.map((p) => {
    const [x, y] = at([p.x, p.y], false);
    const r = p.role === "dl" ? 0.85 : 1.15;
    // End labels extend toward the edge, leaving room for the tightly
    // spaced interior linemen without shrinking any slot names.
    const edge = p.role === "dl" && Math.abs(x) >= 3;
    const anchor = edge ? (x > 0 ? "start" : "end") : "middle";
    const labelX = edge ? x - Math.sign(x) * 0.5 : x;
    return `<g class="art-defender"><title>${esc(p.slot)}</title>` +
      `<circle class="art-player art-${esc(p.role || "other")}" cx="${n(x)}" cy="${n(y)}" r="${r}"/>` +
      `<text class="art-defender-label" x="${n(labelX)}" y="${n(y - r - 0.55)}" text-anchor="${anchor}">${esc(p.slot)}</text></g>`;
  }).join("");
}

// A capsule is a round-ended rectangle along its segment. Use a circle for
// a zero-length segment so the radius remains exact, without a degenerate path.
function zoneShape(a: ArtZone): string {
  if (a.shape === "box") {
    return `<rect class="art-zone art-zone-deep" x="${n(a.drop[0] - a.width / 2)}" y="${n(-a.depth[1])}" ` +
      `width="${n(a.width)}" height="${n(a.depth[1] - a.depth[0])}"/>`;
  }
  const [x, y] = at(a.ends[0], false);
  const dx = a.ends[1][0] - a.ends[0][0];
  const dy = -(a.ends[1][1] - a.ends[0][1]);
  const length = Math.hypot(dx, dy);
  if (length === 0) return `<circle class="art-zone art-zone-under" cx="${n(x)}" cy="${n(y)}" r="${n(a.radius)}"/>`;
  const angle = Math.atan2(dy, dx) * 180 / Math.PI;
  return `<rect class="art-zone art-zone-under" x="${n(-a.radius)}" y="${n(-a.radius)}" ` +
    `width="${n(length + 2 * a.radius)}" height="${n(2 * a.radius)}" rx="${n(a.radius)}" ` +
    `transform="translate(${n(x)} ${n(y)}) rotate(${n(angle)})"/>`;
}

// Find the zone's horizontal section at the top edge. This prevents a
// continuation mark on a capsule whose bounding box alone crosses the card.
function zoneTop(a: ArtZone): [number, number] | null {
  if (a.shape === "box") {
    return a.depth[0] < ART_DEEP && a.depth[1] > ART_DEEP
      ? [a.drop[0] - a.width / 2, a.drop[0] + a.width / 2] : null;
  }
  const sections: number[] = [];
  for (const [x, y] of a.ends) {
    const dy = ART_DEEP - y;
    if (Math.abs(dy) < a.radius) {
      const dx = Math.sqrt(a.radius ** 2 - dy ** 2);
      sections.push(x - dx, x + dx);
    }
  }
  // Intersect the two straight sides of the capsule with the top edge.
  const [p, q] = a.ends;
  const dx = q[0] - p[0], dy = q[1] - p[1];
  const length = Math.hypot(dx, dy);
  if (length > 0 && dy !== 0) {
    for (const sign of [-1, 1]) {
      const ox = -dy / length * a.radius * sign, oy = dx / length * a.radius * sign;
      const t = (ART_DEEP - p[1] - oy) / dy;
      if (t >= 0 && t <= 1) sections.push(p[0] + ox + t * dx);
    }
  }
  return sections.length ? [Math.min(...sections), Math.max(...sections)] : null;
}

function zoneContinuation(a: ArtZone): string {
  const section = zoneTop(a);
  if (!section) return "";
  const left = Math.max(section[0], -ART_HALF_WIDTH + EDGE);
  const right = Math.min(section[1], ART_HALF_WIDTH - EDGE);
  if (right <= left) return "";
  const x = (left + right) / 2;
  return endMark([x, -ART_DEEP + 2], [x, -ART_DEEP + EDGE], "continue",
    `art-zone-edge ${a.shape === "box" ? "art-deep-line" : "art-under-line"}`);
}

function defensiveJob(job: DefensiveArtJob): string {
  let drawing: string;
  let title: string;
  switch (job.kind) {
    case "zone": {
      const clipped = cutAt(job.points, ART_DEEP - EDGE);
      drawing = line(job.points, "settle", `art-zone-drop ${job.area.shape === "box" ? "art-deep-line" : "art-under-line"}`, false) +
        (clipped.cut && clipped.points.length >= 2 ? "" : zoneContinuation(job.area));
      title = `${job.slot}: Zone coverage`;
      break;
    }
    case "man": {
      const [x, y] = at(job.at, false);
      drawing = `<circle class="art-man" cx="${n(x)}" cy="${n(y)}" r="1.65"/>`;
      title = `${job.slot}: Man coverage, ${job.leverage} leverage`;
      break;
    }
    case "spy": {
      const [x, y] = at(job.at, false);
      drawing = `<polygon class="art-spy" points="${pts([[x, y - 2], [x + 2, y], [x, y + 2], [x - 2, y]])}"/>`;
      title = `${job.slot}: Spy`;
      break;
    }
    default:
      drawing = line(job.points, "continue", job.kind === "blitz" ? "art-blitz" : `art-rush${job.contain ? " art-contain" : ""}`, false);
      title = `${job.slot}: ${job.kind === "blitz" ? "Blitz" : "Rush"}${job.contain ? ", contain" : ""}`;
      break;
  }
  return `<g class="art-job"><title>${esc(title)}</title>${drawing}</g>`;
}

export function defensiveFormationSvg(f: DefensiveFormationArt): string {
  return svg(field() + defensivePlayers(f.players), f.name, true);
}

export function defensivePlaySvg(p: DefensivePlayArt): string {
  // Areas first, then every assignment, then markers/labels. An overlapping
  // zone must never paint over another defender or his rush/man symbol.
  let body = field();
  for (const job of p.jobs) if (job.kind === "zone") body += zoneShape(job.area);
  for (const job of p.jobs) body += defensiveJob(job);
  return svg(body + defensivePlayers(p.players), p.name, true);
}
