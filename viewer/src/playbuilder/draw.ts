// The field: where everyone stands (layout, no DOM, unit tested) and the SVG
// drawing of the offense, its routes, blocks, and run path, and the defense
// with its zones, rushes, and fits. In yards, with downfield up.

import {
  endOfLine, gapX, makeFrame, numbersX, hashX, outward, parseAnchor, placeSlots, playerFor, readOffense, resolveSide, resolveX, sidelineX,
  type Anchor, type Ctx, type Frame, type Hash, type OffenseRead, type Side, type Spot,
} from "./anchors";
import * as D from "./defense";
import * as O from "./offense";

export interface RoutePoint { x: number; y: number; type?: string }
export interface Route { points: RoutePoint[]; end: "continue" | "stop" | "settle" }
export interface Zone {
  drop: { x: unknown; y: number };
  width?: number;
  depth?: [number, number];
  ends?: { x: unknown; y: number }[];
  radius?: number;
}
export interface Library {
  routes: Record<string, Route>;
  zones: Record<string, Zone>;
  blocking: { run_schemes: Record<string, unknown>; protections: Record<string, unknown> };
  qb: { drops: Record<string, { points: { x: number; y: number }[] }>; fakes: Record<string, { points: { x: number; y: number }[] }> };
}
export const EMPTY_LIBRARY: Library = { routes: {}, zones: {}, blocking: { run_schemes: {}, protections: {} }, qb: { drops: {}, fakes: {} } };

export interface Scene {
  mode: "offense" | "defense";
  hash: Hash;
  lib: Library;
  off: { formation: O.OffFormation; play: O.OffensePlay };
  def: { formation: D.DefFormation; play: D.DefensePlay; calls: string[]; override: Side | ""; view: "pass" | "run" };
  selected: string; // a slot of the side being edited, or ""
}

export interface P { x: number; y: number }

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

export interface Layout {
  frame: Frame;
  error: string; // what stopped the drawing, or ""
  off: Record<string, P>;
  def: Record<string, P>;
  read?: OffenseRead;
  path: P[]; // the run path's points
}

// The defense's effective alignment for a slot: the play's align, else the formation's.
export const defAnchor = (s: D.DefSlot, p: D.DefensePlay): { x: Anchor; y: number } => {
  const al = p.assignments[s.name]?.align ?? {};
  return { x: al.x ?? s.x, y: al.y ?? s.y };
};

export function layout(sc: Scene): Layout {
  const frame = makeFrame(sc.hash);
  const out: Layout = { frame, error: "", off: {}, def: {}, path: [] };
  const f = sc.off.formation;
  try {
    out.off = placeSlots(f.slots.map((s) => ({ name: s.name, x: s.x, y: s.y })), { frame });
  } catch (e) {
    out.error = `Offense: ${(e as Error).message}`;
    return out;
  }
  const spots: Spot[] = f.slots.map((s) => ({ name: s.name, ...out.off[s.name]!, onLine: s.onLine }));
  let readError = "";
  try {
    out.read = sc.mode === "defense" ? readOffense(spots, frame, sc.def.calls, sc.def.override) : readOffense(spots, frame, [], "");
  } catch (e) {
    readError = (e as Error).message;
  }
  if (sc.mode === "offense") {
    const p = sc.off.play;
    if (p.type === "run") {
      try {
        const ctx: Ctx = { frame, read: out.read, slotPos: (n) => out.off[n] };
        out.path = p.run.path.map((pt) => ({ x: resolveX(pt.x, ctx), y: pt.y }));
      } catch (e) {
        out.error = `Run path: ${(e as Error).message}${readError ? ` (${readError})` : ""}`;
      }
    }
    return out;
  }
  if (!out.read) {
    out.error = `The offense (Offense tab): ${readError}`;
    return out;
  }
  try {
    const eff = sc.def.formation.slots.map((s) => ({ name: s.name, ...defAnchor(s, sc.def.play) }));
    out.def = placeSlots(eff, { frame, read: out.read });
  } catch (e) {
    out.error = `Defense: ${(e as Error).message}`;
  }
  return out;
}

// "#1 strong", "#2 weak", "back 1": what the defense calls an offensive player.
export function receiverLabels(read: OffenseRead): Record<string, string> {
  const out: Record<string, string> = {};
  for (const side of ["left", "right"] as Side[]) {
    const word = side === read.strong ? "s" : "w";
    read.receivers[side].forEach((n, i) => (out[n] = `#${i + 1}${word}`));
  }
  read.backs.forEach((n, i) => (out[n] = `b${i + 1}`));
  return out;
}

// Applies a route's depth override: every point from the first break moves by
// (new depth - the template's break depth).
export function routePoints(route: Route, depth: number | undefined): RoutePoint[] {
  const brk = route.points.findIndex((p) => p.type === "break");
  if (depth === undefined || brk < 0) return route.points;
  const shift = depth - route.points[brk]!.y;
  return route.points.map((p, i) => (i >= brk ? { ...p, y: p.y + shift } : p));
}

// A route as field points starting at the player, +x turned toward his sideline.
export function routePath(route: Route, ref: O.RouteRef, x: number, y: number): { x: number; y: number; brk: boolean }[] {
  const side: Side = ref.side || (x < 0 ? "left" : "right");
  const sign = outward(side);
  return [{ x, y, brk: false }, ...routePoints(route, ref.depth).map((p) => ({ x: x + sign * p.x, y: y + p.y, brk: p.type === "break" }))];
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

const NS = "http://www.w3.org/2000/svg";
const S = <K extends keyof SVGElementTagNameMap>(tag: K, attrs: Record<string, string | number>, text?: string): SVGElementTagNameMap[K] => {
  const e = document.createElementNS(NS, tag);
  for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, String(v));
  if (text !== undefined) e.textContent = text;
  return e;
};
const pts = (list: P[]) => list.map((p) => `${p.x},${-p.y}`).join(" ");

export const Y_TOP = 30;
export const Y_BOTTOM = -13;
const X_PAD = 1;
const YELLOW = "#ffd400";
const GREEN = "#7bd88f";
const RED = "#ff6b6b";

// Defense tuning the drawing needs (data/tuning/defense.json, blocking.json).
const ALLEY: [number, number] = [3, 4];
const FORCE: [number, number] = [5, 2];
const DEEP_FILL = 8;
const CONTAIN_OUTSIDE = 1.5; // pass rush contain (blocking.json)
const RUN_CONTAIN_OUTSIDE = 1; // run contain (defense.json)

// Places text labels so they don't sit on each other: each new label moves
// down (then up) in small steps until its box is clear.
class Labels {
  private boxes: { x0: number; x1: number; y0: number; y1: number }[] = [];
  constructor(private svg: SVGSVGElement) {}
  // `down`: only try lower spots (labels under the line of scrimmage).
  add(x: number, y: number, text: string, color: string, size = 0.75, anchor: "start" | "middle" = "start", opacity = 1, down = false): void {
    const w = text.length * size * 0.52;
    const x0 = anchor === "middle" ? x - w / 2 : x;
    const sy = -y; // svg y of the baseline
    const steps = down ? [0, 1, 2, 3, 4, 5, 6, 7, 8] : [0, 1, -1, 2, -2, 3, -3, 4, -4, 5, -5];
    for (const step of steps) {
      const b = { x0, x1: x0 + w, y0: sy - size + step * size * 1.1, y1: sy + 0.15 + step * size * 1.1 };
      if (this.boxes.some((o) => o.x0 < b.x1 && b.x0 < o.x1 && o.y0 < b.y1 && b.y0 < o.y1)) continue;
      this.boxes.push(b);
      this.svg.append(S("text", { x, y: b.y1 - 0.15, "font-size": size, fill: color, "text-anchor": anchor, opacity, class: "label" }, text));
      return;
    }
  }
  reserve(x: number, y: number, r: number): void {
    this.boxes.push({ x0: x - r, x1: x + r, y0: -y - r, y1: -y + r });
  }
}

function field(svg: SVGSVGElement, frame: Frame): void {
  const left = sidelineX(frame, "left");
  const right = sidelineX(frame, "right");
  svg.setAttribute("viewBox", `${left - X_PAD} ${-Y_TOP} ${right - left + 2 * X_PAD} ${Y_TOP - Y_BOTTOM}`);
  svg.append(S("rect", { x: left, y: -Y_TOP, width: right - left, height: Y_TOP - Y_BOTTOM, fill: "#20402a" }));
  const line = (x1: number, y1: number, x2: number, y2: number, stroke: string, w: number) => svg.append(S("line", { x1, y1: -y1, x2, y2: -y2, stroke, "stroke-width": w }));
  for (let y = Math.ceil(Y_BOTTOM); y <= Y_TOP; y++) {
    if (y % 5 === 0 && y !== 0) line(left, y, right, y, "#ffffff40", 0.1);
    for (const x of [hashX(frame, "left"), hashX(frame, "right")]) line(x - 0.4, y, x + 0.4, y, "#ffffff40", 0.08);
  }
  for (const s of ["left", "right"] as Side[]) {
    line(sidelineX(frame, s), Y_BOTTOM, sidelineX(frame, s), Y_TOP, "#ffffffaa", 0.25);
    for (const y of [-10, 10, 20]) svg.append(S("text", { x: numbersX(frame, s), y: -y + 0.7, "font-size": 2, fill: "#ffffff22", "text-anchor": "middle", "font-weight": "bold" }, String(y > 0 ? y : 50 + y)));
  }
  for (let y = 5; y < Y_TOP; y += 5) svg.append(S("text", { x: right - 0.4, y: -y - 0.2, "font-size": 0.7, fill: "#ffffff55", "text-anchor": "end" }, `${y}`));
  line(left, 0, right, 0, YELLOW, 0.2);
  svg.append(S("text", { x: left + 0.4, y: -0.4, "font-size": 0.8, fill: YELLOW }, "LOS"));
}

function arrowHead(svg: Element, a: P, b: P, color: string, w = 0.22): void {
  const ang = Math.atan2(-(b.y - a.y), b.x - a.x);
  const arm = (d: number) => `${b.x - Math.cos(ang + d)},${-b.y - Math.sin(ang + d)}`;
  svg.append(S("polyline", { points: `${arm(0.45)} ${b.x},${-b.y} ${arm(-0.45)}`, fill: "none", stroke: color, "stroke-width": w }));
}

function drawRoute(g: Element, labels: Labels, route: Route, ref: O.RouteRef, x: number, y: number, color: string, dash: string | null, label: string, w: number): void {
  const path = routePath(route, ref, x, y);
  g.append(S("polyline", { points: pts(path), fill: "none", stroke: color, "stroke-width": w, "stroke-linejoin": "round", ...(dash ? { "stroke-dasharray": dash } : {}) }));
  for (const p of path.slice(1, -1)) if (p.brk) g.append(S("circle", { cx: p.x, cy: -p.y, r: 0.3, fill: color }));
  const a = path[path.length - 2]!;
  const b = path[path.length - 1]!;
  if (route.end === "continue") arrowHead(g, a, b, color, w);
  else if (route.end === "stop") {
    const ang = Math.atan2(-(b.y - a.y), b.x - a.x);
    const [dx, dy] = [-Math.sin(ang) * 0.8, Math.cos(ang) * 0.8];
    g.append(S("line", { x1: b.x - dx, y1: -b.y - dy, x2: b.x + dx, y2: -b.y + dy, stroke: color, "stroke-width": w + 0.08 }));
  } else g.append(S("circle", { cx: b.x, cy: -b.y, r: 0.55, fill: "none", stroke: color, "stroke-width": w }));
  // The name goes above the route's deepest point, clear of the route itself.
  const high = path.reduce((m, q) => (q.y > m.y ? q : m), path[0]!);
  labels.add(high.x, Math.min(high.y + 0.9, Y_TOP - 1.2), label, color, 0.75, "middle");
}

const OFF_COLORS: Record<O.Role, string> = { ol: "#9aa3ad", qb: RED, back: "#5aa9ff", wr: "#5aa9ff", te: "#5aa9ff" };
const DEF_COLORS: Record<D.DefRole, string> = { dl: "#ff9f43", lb: "#f9c74f", db: "#c792ea" };

function player(svg: SVGSVGElement, labels: Labels, name: string, fill: string, p: P, team: "off" | "def" | "ghost", selected: boolean, title: string): void {
  const g = S("g", { class: team === "ghost" ? "ghost" : "piece", ...(team !== "ghost" ? { "data-slot": name } : {}) });
  if (selected) g.append(S("circle", { cx: p.x, cy: -p.y, r: 1.45, fill: "none", stroke: "#fff", "stroke-width": 0.2 }));
  g.append(S("circle", { cx: p.x, cy: -p.y, r: 0.95, fill, stroke: "#000", "stroke-width": 0.1, opacity: team === "ghost" ? 0.35 : 1 }));
  const size = name.length <= 2 ? 0.8 : name.length === 3 ? 0.62 : 0.5;
  g.append(S("text", { x: p.x, y: -p.y + size * 0.36, "font-size": size, fill: "#000", "text-anchor": "middle", "font-weight": "bold", opacity: team === "ghost" ? 0.5 : 1 }, name));
  g.append(S("title", {}, title));
  svg.append(g);
  labels.reserve(p.x, p.y, 1);
}

// The whole picture. `lay` is layout(sc).
export function draw(svg: SVGSVGElement, sc: Scene, lay: Layout): void {
  svg.replaceChildren();
  field(svg, lay.frame);
  const labels = new Labels(svg);
  const f = sc.off.formation;
  if (Object.keys(lay.off).length === 0) return;
  if (sc.mode === "offense") {
    for (const s of f.slots) labels.reserve(lay.off[s.name]!.x, lay.off[s.name]!.y, 1);
    drawOffense(svg, labels, sc, lay);
    for (const s of f.slots) player(svg, labels, s.name, OFF_COLORS[O.roleOf(s.pos)], lay.off[s.name]!, "off", sc.selected === s.name, offTitle(s, sc.off.play));
    return;
  }
  const names = lay.read ? receiverLabels(lay.read) : {};
  for (const s of f.slots) {
    const p = lay.off[s.name]!;
    player(svg, labels, s.name, OFF_COLORS[O.roleOf(s.pos)], p, "ghost", false, `${s.name} (${s.pos})${names[s.name] ? `: ${names[s.name]}` : ""}`);
    if (names[s.name]) labels.add(p.x, p.y - 1.9, names[s.name]!, "#ffffffaa", 0.65, "middle");
  }
  if (!lay.read || Object.keys(lay.def).length === 0) return;
  drawDefense(svg, labels, sc, lay, lay.read);
}

function offTitle(s: O.OffSlot, p: O.OffensePlay): string {
  const a = O.currentAssignments(p)[s.name];
  return `${s.name} (${s.pos}): ${a ? describeOff(a, p) : "no assignment"}\nDrag to move, click to edit.`;
}

export function describeOff(a: O.OffAssign, p: O.OffensePlay): string {
  const route = (r: O.RouteJob) => `${r.route}${r.depth !== undefined ? ` @${r.depth}` : ""}${r.vsMan ? `, vs man ${r.vsMan.route}` : ""}${r.vsZone ? `, vs zone ${r.vsZone.route}` : ""}`;
  if (a.kind === "none") return p.type === "run" ? "carries / hands off" : "passer";
  if (a.kind === "route") return route(a.route);
  if (a.then) return `${a.block.action}, then ${route(a.then)}`;
  if (a.block.action === "protect") return `protect (${p.pass.protection})`;
  if (a.block.action === "scheme") return `scheme (${p.run.scheme})`;
  return `${a.block.action}${a.block.target ? ` ${a.block.target}` : ""}`;
}

function drawOffense(svg: SVGSVGElement, labels: Labels, sc: Scene, lay: Layout): void {
  const { formation: f, play: p } = sc.off;
  const lib = sc.lib;
  const map = O.currentAssignments(p);
  const routesG = S("g", {});
  svg.append(routesG);
  const blockers = new Map<string, P[]>(); // label -> who blocks that way
  // The primary target (the first read) runs his route in red, like a play card.
  const primary = p.type === "pass" ? p.pass.progression[0] : undefined;
  for (const s of f.slots) {
    const at = lay.off[s.name]!;
    const a = map[s.name];
    if (!a || a.kind === "none") continue;
    const w = sc.selected === s.name ? 0.34 : 0.22;
    const color = s.name === primary ? RED : YELLOW;
    const jobs: { r: O.RouteJob; prefix: string }[] = [];
    if (a.kind === "route") jobs.push({ r: a.route, prefix: "" });
    else if (a.then) jobs.push({ r: a.then, prefix: "chk → " });
    else {
      // A puller: an arrow along the line to the play side.
      if ((a.block.action === "pull_kick" || a.block.action === "pull_lead") && lay.read && p.type === "run") {
        const ps = p.run.direction;
        const to = { x: endOfLine(lay.read, ps) + outward(ps) * (a.block.action === "pull_kick" ? 1.5 : 0.5), y: at.y - 1.2 };
        const knee = { x: at.x, y: at.y - 1.2 };
        routesG.append(S("polyline", { points: pts([at, knee, to, { x: to.x, y: to.y + 2.5 }]), fill: "none", stroke: "#fff", "stroke-width": 0.18, "stroke-dasharray": "0.4 0.25", opacity: 0.8 }));
        arrowHead(routesG, to, { x: to.x, y: to.y + 2.5 }, "#fff", 0.18);
      }
      const label = a.block.action === "protect" ? `protect (${p.pass.protection})` : a.block.action === "scheme" ? `${p.run.scheme}` : `${a.block.action}${a.block.target ? ` ${a.block.target}` : ""}`;
      blockers.set(label, [...(blockers.get(label) ?? []), at]);
    }
    for (const { r, prefix } of jobs) {
      const route = lib.routes[r.route];
      if (!route) continue;
      drawRoute(routesG, labels, route, r, at.x, at.y, color, null, prefix + r.route, w);
      if (r.vsMan && lib.routes[r.vsMan.route]) drawRoute(routesG, labels, lib.routes[r.vsMan.route]!, { side: r.side, ...r.vsMan }, at.x, at.y, "#ff9f43", "0.5 0.4", `vs man: ${r.vsMan.route}`, 0.18);
      if (r.vsZone && lib.routes[r.vsZone.route]) drawRoute(routesG, labels, lib.routes[r.vsZone.route]!, { side: r.side, ...r.vsZone }, at.x, at.y, "#4dd0e1", "0.5 0.4", `vs zone: ${r.vsZone.route}`, 0.18);
    }
  }
  // One label under each run of neighbors that block the same way.
  for (const [label, who] of blockers) {
    const xs = who.map((q) => q).sort((a, b) => a.x - b.x);
    const groups: P[][] = [];
    for (const q of xs) {
      const g = groups[groups.length - 1];
      if (g && q.x - g[g.length - 1]!.x <= 3) g.push(q);
      else groups.push([q]);
    }
    for (const g of groups) {
      const lo = g[0]!.x;
      const hi = g[g.length - 1]!.x;
      const y = Math.min(...g.map((q) => q.y)) - 1.5;
      if (g.length > 1) svg.append(S("line", { x1: lo, y1: -y, x2: hi, y2: -y, stroke: "#fff", "stroke-width": 0.1, opacity: 0.6 }));
      labels.add((lo + hi) / 2, y - 0.9, label, "#ffffffcc", 0.7, "middle", 1, true);
    }
  }
  const qbSlot = f.slots.find(O.isPasser);
  const qb = qbSlot ? lay.off[qbSlot.name] : undefined;
  if (p.type === "pass") {
    const drop = lib.qb.drops[p.pass.drop];
    if (drop && qb) svg.append(S("polyline", { points: pts([qb, ...drop.points]), fill: "none", stroke: RED, "stroke-width": 0.2, "stroke-dasharray": "0.3 0.3" }));
    // The read order and the hot route, as badges on the receivers.
    p.pass.progression.forEach((n, i) => {
      const at = lay.off[n];
      if (!at) return;
      svg.append(S("circle", { cx: at.x + 0.95, cy: -at.y - 0.95, r: 0.5, fill: YELLOW, stroke: "#000", "stroke-width": 0.06 }));
      svg.append(S("text", { x: at.x + 0.95, y: -at.y - 0.75, "font-size": 0.6, "text-anchor": "middle", fill: "#000", "font-weight": "bold" }, String(i + 1)));
    });
    const hot = p.pass.hot ? lay.off[p.pass.hot] : undefined;
    if (hot) {
      svg.append(S("rect", { x: hot.x - 1.9, y: -hot.y - 1.4, width: 1.6, height: 0.8, rx: 0.2, fill: RED }));
      svg.append(S("text", { x: hot.x - 1.1, y: -hot.y - 0.78, "font-size": 0.55, "text-anchor": "middle", fill: "#000", "font-weight": "bold" }, "HOT"));
    }
    return;
  }
  const carrier = lay.off[p.run.carrier];
  if (!carrier || lay.path.length !== p.run.path.length) return;
  const line = [carrier, ...lay.path];
  svg.append(S("polyline", { points: pts(line), fill: "none", stroke: GREEN, "stroke-width": 0.28, "stroke-dasharray": "0.6 0.3" }));
  if (line.length > 1) arrowHead(svg, line[line.length - 2]!, line[line.length - 1]!, GREEN, 0.28);
  lay.path.forEach((pt, i) => {
    const handle = S("g", { class: "piece", "data-path": i });
    handle.append(S("circle", { cx: pt.x, cy: -pt.y, r: 0.5, fill: GREEN, stroke: "#000", "stroke-width": 0.08 }));
    handle.append(S("title", {}, `Path point ${i + 1}${p.run.path[i]!.type ? ` (${p.run.path[i]!.type})` : ""}: drag to move`));
    svg.append(handle);
    labels.add(pt.x + 0.8, pt.y + 0.5, p.run.path[i]!.type || `pt ${i + 1}`, GREEN, 0.65);
  });
  if (qb && p.run.then) {
    const fake = lib.qb.fakes[p.run.then];
    if (fake) svg.append(S("polyline", { points: pts([qb, ...fake.points]), fill: "none", stroke: RED, "stroke-width": 0.2, "stroke-dasharray": "0.3 0.3" }));
  }
}

function zoneShape(g: Element, z: Zone, ctx: Ctx, color: string): P {
  const at = (p: { x: unknown; y: number }) => ({ x: resolveX(parseAnchor(p.x), ctx), y: p.y });
  const drop = at(z.drop);
  if (z.width !== undefined && z.depth) {
    const top = Math.min(z.depth[1], Y_TOP);
    g.append(S("rect", { x: drop.x - z.width / 2, y: -top, width: z.width, height: top - z.depth[0], fill: color, opacity: 0.16, stroke: color, "stroke-width": 0.1 }));
  } else if (z.radius !== undefined) {
    const ends = z.ends?.map(at) ?? [drop];
    const a = ends[0]!;
    const b = ends[ends.length - 1]!;
    g.append(S("line", { x1: a.x, y1: -a.y, x2: b.x, y2: -b.y, stroke: color, "stroke-width": 2 * z.radius, "stroke-linecap": "round", opacity: 0.16 }));
  }
  return drop;
}

const COLORS = ["#ff9f43", "#4dd0e1", "#c792ea", "#7bd88f", "#ff6b6b", "#f9e2af", "#89b4fa", "#f38ba8", "#a6e3a1", "#fab387", "#94e2d5", "#cba6f7"];

export function describeRun(j: D.RunJob): string {
  return j.kind === "gap" ? `${j.gap} gap ${j.side}` : j.kind === "support" ? j.ref : j.kind;
}
export function describePass(j: D.PassJob): string {
  switch (j.kind) {
    case "rush": return `rush ${j.lane}${j.side ? ` ${j.side}` : ""}`;
    case "zone": return j.ref;
    case "man": return `man ${j.target}${j.leverage === "outside" ? " (outside)" : ""}`;
    case "blitz": return `blitz ${j.gap} ${j.side}`;
    case "stunt": return `stunt ${j.lane}`;
    case "spy": return "spy";
  }
}

function drawDefense(svg: SVGSVGElement, labels: Labels, sc: Scene, lay: Layout, read: OffenseRead): void {
  const def = sc.def;
  const placed = lay.def;
  const ctx: Ctx = { frame: lay.frame, read, slotPos: (n) => placed[n] };
  const sideOf = (x: number): Side => (x < read.centerX ? "left" : "right");
  const strong = read.strong;
  svg.append(S("text", { x: read.centerX, y: -Y_TOP + 1.6, "font-size": 0.9, fill: "#fff", "text-anchor": "middle", opacity: 0.85 }, `strong side: ${strong}${def.override ? " (forced)" : ` (${def.calls.join(" → ") || "no calls"})`}`));
  const anySelected = def.formation.slots.some((s) => s.name === sc.selected);
  const zoneG = S("g", {});
  const jobG = S("g", {});
  svg.append(zoneG, jobG);

  def.formation.slots.forEach((s, i) => {
    const p = placed[s.name]!;
    const a = def.play.assignments[s.name];
    if (!a) return;
    const color = COLORS[i % COLORS.length]!;
    const mine = sc.selected === s.name;
    const op = anySelected && !mine ? 0.3 : 1;
    const g = S("g", { opacity: op });
    const zg = S("g", { opacity: op });
    jobG.append(g);
    zoneG.append(zg);
    const w = mine ? 0.32 : 0.2;
    const arrow = (to: P, dash: string | null, label: string) => {
      g.append(S("line", { x1: p.x, y1: -p.y, x2: to.x, y2: -to.y, stroke: color, "stroke-width": w, ...(dash ? { "stroke-dasharray": dash } : {}) }));
      arrowHead(g, p, to, color, w);
      if (mine && label) labels.add(to.x + 0.6, to.y + 0.6, label, color, 0.8);
    };
    const side = (word: string): Side => resolveSide((word || sideOf(p.x)) as never, lay.frame, read);
    const zoneLabel = (ref: string, z: Zone, at: P) => {
      const top = z.depth ? Math.min(z.depth[0] + 2, Y_TOP - 2) : at.y + (z.radius ?? 0) * 0.6;
      labels.add(at.x, top, ref, color, 0.65, "middle", op === 1 ? 0.9 : 0.35);
    };
    if (def.view === "run") {
      const j = a.run;
      if (j.kind === "gap") arrow({ x: gapX(read, side(j.side), j.gap), y: 0.5 }, null, describeRun(j));
      else if (j.kind === "support") {
        if (j.ref === "deep_fill") arrow({ x: read.centerX, y: DEEP_FILL }, "0.4 0.3", "deep fill");
        else if (j.ref.startsWith("deep_third")) {
          const z = a.pass.kind === "zone" ? sc.lib.zones[a.pass.ref] : undefined;
          if (z) arrow(zoneShape(zg, z, ctx, color), "0.4 0.3", "deep third (his zone)");
        } else {
          const sd = j.ref.endsWith("_strong") ? strong : j.ref.endsWith("_weak") ? (strong === "left" ? "right" : "left") : sideOf(p.x);
          const spot = j.ref.startsWith("force") ? FORCE : ALLEY;
          arrow({ x: endOfLine(read, sd) + outward(sd) * spot[0], y: spot[1] }, "0.4 0.3", j.ref);
        }
      } else if (j.kind === "contain") {
        const sd = sideOf(p.x);
        arrow({ x: endOfLine(read, sd) + outward(sd) * RUN_CONTAIN_OUTSIDE, y: 2 }, null, "contain");
      } else if (mine) labels.add(p.x, p.y + 1.8, "pursue", color, 0.75, "middle");
    } else {
      const j = a.pass;
      switch (j.kind) {
        case "zone": {
          const z = sc.lib.zones[j.ref];
          if (z) {
            const at = zoneShape(zg, z, ctx, color);
            arrow(at, "0.4 0.3", "");
            zoneLabel(j.ref, z, at);
          }
          break;
        }
        case "man": {
          const name = playerFor(read, j.target);
          const t = name ? read.pos[name] : undefined;
          const dir = t && t.x < read.centerX ? -1 : 1; // toward his sideline
          if (t) arrow({ x: t.x + (j.leverage === "outside" ? dir : -dir) * 0.8, y: t.y + 0.9 }, "0.3 0.2", describePass(j));
          else {
            const z = sc.lib.zones.hole;
            if (z) arrow(zoneShape(zg, z, ctx, color), "0.3 0.2", `${j.target}: nobody there, plays the hole`);
          }
          break;
        }
        case "rush":
        case "blitz":
        case "stunt": {
          const lane = j.kind === "blitz" ? j.gap : j.lane;
          const sd = side(j.kind === "stunt" ? "" : j.side);
          const x = lane === "contain" ? endOfLine(read, sd) + outward(sd) * CONTAIN_OUTSIDE : gapX(read, sd, lane as "A");
          arrow({ x, y: -3.5 }, j.kind === "rush" ? null : "0.5 0.25", describePass(j));
          break;
        }
        case "spy":
          arrow({ x: read.centerX, y: 5 }, "0.3 0.3", "spy");
          break;
      }
    }
  });
  def.formation.slots.forEach((s) => {
    const a = def.play.assignments[s.name];
    const al = a && (a.align.x !== undefined || a.align.y !== undefined) ? "\nThis call moves him (align)." : "";
    const title = `${s.name} (${s.pos})${a ? `\nrun: ${describeRun(a.run)}\npass: ${describePass(a.pass)}` : ""}${al}\nDrag to move, click to edit.`;
    player(svg, labels, s.name, DEF_COLORS[D.defRoleOf(s.pos)], placed[s.name]!, "def", sc.selected === s.name, title);
  });
}
