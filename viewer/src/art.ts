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
  team: string;
  playbook: string;
  formations: FormationArt[];
  plays: PlayArt[];
}

// The server's {"type": "playbook_art", "team", "art"} (or "error"): the side
// and its art, or the side and why there's none.
export function readArtMessage(m: unknown): { side: TeamKey; art: PlaybookArt | null; error: string } | null {
  if (!m || typeof m !== "object") return null;
  const o = m as Record<string, unknown>;
  if (o.type !== "playbook_art" || (o.team !== "home" && o.team !== "away")) return null;
  const art = o.art as PlaybookArt | undefined;
  if (art && Array.isArray(art.formations) && Array.isArray(art.plays)) return { side: o.team, art, error: "" };
  return { side: o.team, art: null, error: typeof o.error === "string" ? o.error : "no art in the message" };
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

function svg(body: string, title: string): string {
  const w = ART_HALF_WIDTH;
  return (
    `<svg class="play-art" xmlns="http://www.w3.org/2000/svg" viewBox="${n(-w)} ${-ART_DEEP} ${n(2 * w)} ${ART_DEEP + ART_BACK}" ` +
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
