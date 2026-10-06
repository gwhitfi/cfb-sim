// Anchors and the offense read: a TypeScript port of the parts of
// sim/src/field/geometry.cpp the play builder needs (docs/cfbsim_design.md,
// "Position anchors" and "Reading the offense"). No DOM, so it is unit tested.
//
// Coordinates are the play frame: x right of the ball, y downfield of the line.
// Plays are never flipped here; the ball sits on the hash picked in the page.

export type Side = "left" | "right";
export type SideWord = Side | "strong" | "weak" | "field" | "boundary";
export const SIDE_WORDS: readonly SideWord[] = ["left", "right", "strong", "weak", "field", "boundary"];

export const HALF_WIDTH = 160 / 6; // sideline to the middle of the field, 26.67 yd
export const HASH_FROM_SIDELINE = 20;
export const NUMBERS_FROM_SIDELINE = 8;

export type Hash = "left" | "middle" | "right";

export interface Frame {
  ballX: number; // the ball's x on the field: 0 in the middle, -6.67 on the left hash
}
export function makeFrame(hash: Hash): Frame {
  const off = HALF_WIDTH - HASH_FROM_SIDELINE;
  return { ballX: hash === "left" ? -off : hash === "right" ? off : 0 };
}
export const sidelineX = (f: Frame, s: Side) => (s === "left" ? -HALF_WIDTH - f.ballX : HALF_WIDTH - f.ballX);
export const hashX = (f: Frame, s: Side) => sidelineX(f, s) + (s === "left" ? HASH_FROM_SIDELINE : -HASH_FROM_SIDELINE);
export const numbersX = (f: Frame, s: Side) => sidelineX(f, s) + (s === "left" ? NUMBERS_FROM_SIDELINE : -NUMBERS_FROM_SIDELINE);
export const fieldSide = (f: Frame): Side => (sidelineX(f, "right") >= -sidelineX(f, "left") ? "right" : "left");
export const mirrored = (s: Side): Side => (s === "left" ? "right" : "left");
export const outward = (s: Side) => (s === "left" ? -1 : 1);

export const TECHNIQUES = ["0", "1", "2i", "2", "3", "4i", "4", "5", "7", "6", "9"] as const;
export const RECEIVERS = ["#1", "#2", "#3", "#4"] as const;
export const GAPS = ["A", "B", "C", "D"] as const;
export type Gap = (typeof GAPS)[number];

const SHADE = 0.5; // data/tuning alignment: a shade is half a yard
const GHOST_TE_SPLIT = 1.5; // a TE assumed this far outside the tackle when none is attached

export type Anchor =
  | { kind: "number"; value: number }
  | { kind: "ball"; offset: number }
  | { kind: "numbers" | "hash" | "sideline"; side: SideWord; offset: number }
  | { kind: "slot"; ref: string; offset: number }
  | { kind: "gap"; ref: Gap; side: SideWord; offset: number }
  | { kind: "tech"; ref: string; side: SideWord; offset: number }
  | { kind: "over"; ref: string; side: SideWord; offset: number };
export type AnchorKind = Anchor["kind"];

export const num = (value: number): Anchor => ({ kind: "number", value });

// The anchor as the data files write it.
export function anchorJson(a: Anchor): unknown {
  const off = (o: number) => (o ? { offset: o } : {});
  switch (a.kind) {
    case "number":
      return a.value;
    case "ball":
      return { from: "ball", offset: a.offset }; // the data files write the 0
    case "numbers":
    case "hash":
    case "sideline":
      return { from: a.kind, side: a.side, offset: a.offset };
    case "slot":
      return { from: a.ref, ...off(a.offset) };
    case "gap":
      return { from: `gap_${a.ref}`, side: a.side, ...off(a.offset) };
    case "tech":
      return { tech: a.ref, side: a.side, ...off(a.offset) };
    case "over":
      return { over: a.ref, side: a.side, ...off(a.offset) };
  }
}

// An anchor from a data file (zones.json writes them).
export function parseAnchor(j: unknown): Anchor {
  if (typeof j === "number") return num(j);
  if (typeof j !== "object" || j === null) throw new Error(`expected a number or an anchor, got ${JSON.stringify(j)}`);
  const o = j as Record<string, unknown>;
  const offset = typeof o.offset === "number" ? o.offset : 0;
  const side = (o.side as SideWord | undefined) ?? "right";
  if (typeof o.tech === "string") return { kind: "tech", ref: o.tech, side, offset };
  if (typeof o.over === "string") return { kind: "over", ref: o.over, side, offset };
  if (typeof o.from !== "string") throw new Error(`an anchor needs 'from', 'tech', or 'over': ${JSON.stringify(j)}`);
  const from = o.from;
  if (from === "ball") return { kind: "ball", offset };
  if (from === "numbers" || from === "hash" || from === "sideline") return { kind: from, side, offset };
  if (/^gap_[A-D]$/.test(from)) return { kind: "gap", ref: from.slice(4) as Gap, side, offset };
  return { kind: "slot", ref: from, offset };
}

// ---------------------------------------------------------------------------
// Reading the offense
// ---------------------------------------------------------------------------

export interface Spot {
  name: string;
  x: number;
  y: number;
  onLine: boolean;
}

export interface OffenseRead {
  centerX: number;
  line: Record<Side, number[]>; // G, T, and an attached TE, inside out
  receivers: Record<Side, string[]>; // slot names, the widest first
  backs: string[]; // the shallowest first
  pos: Record<string, { x: number; y: number }>;
  strong: Side;
}

const MAX_SPLIT = 2.5;

// Walks outward from the center: each man within 2.5 yd of the last is on the
// line (G, T, then an attached TE).
function walkLine(spots: Spot[], centerX: number, side: Side): Spot[] {
  const step = outward(side);
  const men: Spot[] = [];
  let last = centerX;
  while (men.length < 3) {
    let next: Spot | undefined;
    for (const s of spots) {
      if (!s.onLine || (s.x - last) * step <= 1e-6) continue;
      if (!next || Math.abs(s.x - last) < Math.abs(next.x - last)) next = s;
    }
    if (!next || Math.abs(next.x - last) > MAX_SPLIT) break;
    men.push(next);
    last = next.x;
  }
  if (men.length < 2) throw new Error(`the line needs a guard and a tackle on the ${side}`);
  return men;
}

export const STRENGTH_CALLS = ["te", "passing_strength", "field", "boundary", "back"] as const;

export function readOffense(spots: Spot[], frame: Frame, calls: readonly string[], override: Side | ""): OffenseRead {
  const center = spots.find((s) => s.name === "C");
  const qb = spots.find((s) => s.name === "QB");
  if (!center) throw new Error("the offense needs a C slot");
  const centerX = center.x;
  const interior = new Set<string>(["C"]);
  const line = { left: [] as number[], right: [] as number[] };
  for (const side of ["left", "right"] as Side[]) {
    const men = walkLine(spots, centerX, side);
    line[side] = men.map((m) => m.x);
    for (const m of men.slice(0, 2)) interior.add(m.name);
  }
  const sides: Record<Side, [number, string][]> = { left: [], right: [] };
  const backs: [number, string][] = [];
  for (const s of spots) {
    if (s.name === qb?.name || interior.has(s.name)) continue;
    const dx = s.x - centerX;
    if (s.y < -3 && Math.abs(dx) < 4) backs.push([-s.y, s.name]);
    else sides[dx < 0 ? "left" : "right"].push([-Math.abs(dx), s.name]);
  }
  const byKey = (a: [number, string], b: [number, string]) => a[0] - b[0];
  const read: OffenseRead = {
    centerX,
    line,
    receivers: { left: sides.left.sort(byKey).map((r) => r[1]), right: sides.right.sort(byKey).map((r) => r[1]) },
    backs: backs.sort(byKey).map((b) => b[1]),
    pos: Object.fromEntries(spots.map((s) => [s.name, { x: s.x, y: s.y }])),
    strong: "right",
  };
  read.strong = override || strengthFromCall(read, frame, calls);
  return read;
}

export function strengthFromCall(o: OffenseRead, frame: Frame, calls: readonly string[]): Side {
  for (const call of calls) {
    if (call === "te") {
      const l = o.line.left.length === 3, r = o.line.right.length === 3;
      if (l !== r) return l ? "left" : "right";
    } else if (call === "passing_strength") {
      const l = o.receivers.left.length, r = o.receivers.right.length;
      if (l !== r) return l > r ? "left" : "right";
    } else if (call === "field") return fieldSide(frame);
    else if (call === "boundary") return mirrored(fieldSide(frame));
    else if (call === "back") {
      const b = o.backs[0];
      if (b !== undefined) {
        const dx = o.pos[b]!.x - o.centerX;
        if (Math.abs(dx) > 0.5) return dx < 0 ? "left" : "right";
      }
    }
  }
  return fieldSide(frame);
}

export const endOfLine = (o: OffenseRead, side: Side) => o.line[side][o.line[side].length - 1]!;

// x of gap A..D on a side, from where the linemen stand.
export function gapX(o: OffenseRead, side: Side, letter: Gap): number {
  const men = o.line[side];
  const step = outward(side);
  const [g, t] = [men[0]!, men[1]!];
  const te = men[2];
  const gaps = [(o.centerX + g) / 2, (g + t) / 2, te === undefined ? t + step : (t + te) / 2, te === undefined ? t + 2 * step : te + step];
  return gaps[GAPS.indexOf(letter)]!;
}

// "#2_strong", "#1_weak", "back_1": the slot name of that player, if there is one.
export function playerFor(o: OffenseRead, target: string): string | undefined {
  if (target.startsWith("back_")) return o.backs[Number(target.slice(5)) - 1];
  const m = /^#(\d)_(strong|weak|left|right)$/.exec(target);
  if (!m) return undefined;
  const side = resolveSide(m[2] as SideWord, undefined, o);
  return o.receivers[side][Number(m[1]) - 1];
}

// ---------------------------------------------------------------------------
// Resolving anchors
// ---------------------------------------------------------------------------

export interface Ctx {
  frame: Frame;
  read?: OffenseRead;
  slotPos: (name: string) => { x: number; y: number } | undefined;
}

export function resolveSide(side: SideWord, frame: Frame | undefined, read: OffenseRead | undefined): Side {
  switch (side) {
    case "left":
    case "right":
      return side;
    case "field":
    case "boundary":
      if (!frame) throw new Error("field and boundary need the ball's hash");
      return side === "field" ? fieldSide(frame) : mirrored(fieldSide(frame));
    case "strong":
    case "weak":
      if (!read) throw new Error("strong and weak need the offense's set");
      return side === "strong" ? read.strong : mirrored(read.strong);
  }
}

const isRelative = (s: SideWord) => s !== "left" && s !== "right";

function techniqueX(tech: string, side: Side, o: OffenseRead): number {
  const out = outward(side);
  const line = o.line[side];
  const [g, t] = [line[0]!, line[1]!];
  const te = line[2] ?? t + out * GHOST_TE_SPLIT;
  switch (tech) {
    case "0": return o.centerX;
    case "1": return (o.centerX + g) / 2;
    case "2i": return g - out * SHADE;
    case "2": return g;
    case "3": return g + out * SHADE;
    case "4i": return t - out * SHADE;
    case "4": return t;
    case "5": return t + out * SHADE;
    case "7": return te - out * SHADE;
    case "6": return te;
    case "9": return te + out * SHADE;
  }
  throw new Error(`unknown technique '${tech}'`);
}

// Receiver #n's x: a missing one is halfway between the last receiver and the
// end of the line, or on the numbers if that side has no receivers.
function receiverX(n: number, side: Side, o: OffenseRead, frame: Frame): number {
  const recs = o.receivers[side];
  const one = recs[n - 1];
  if (one !== undefined) return o.pos[one]!.x;
  const last = recs[recs.length - 1];
  if (last === undefined) return numbersX(frame, side);
  return (o.pos[last]!.x + endOfLine(o, side)) / 2;
}

export function resolveX(a: Anchor, ctx: Ctx): number {
  switch (a.kind) {
    case "number":
      return a.value;
    case "ball":
      return a.offset;
    case "numbers":
    case "hash":
    case "sideline": {
      const side = resolveSide(a.side, ctx.frame, ctx.read);
      const f = a.kind === "numbers" ? numbersX : a.kind === "hash" ? hashX : sidelineX;
      return f(ctx.frame, side) + (isRelative(a.side) ? outward(side) : 1) * a.offset;
    }
    case "slot": {
      const p = ctx.slotPos(a.ref);
      if (!p) throw new Error(`slot '${a.ref}' isn't placed`);
      return p.x + a.offset;
    }
    case "gap": {
      if (!ctx.read) throw new Error("gap anchors need the offense's line");
      return gapX(ctx.read, resolveSide(a.side, ctx.frame, ctx.read), a.ref) + a.offset;
    }
    case "tech":
    case "over": {
      if (!ctx.read) throw new Error("defensive anchors need the offense's set");
      const side = resolveSide(a.side, ctx.frame, ctx.read);
      const base = a.kind === "tech" ? techniqueX(a.ref, side, ctx.read) : receiverX(Number(a.ref[1]), side, ctx.read, ctx.frame);
      return base + outward(side) * a.offset;
    }
  }
}

// The same kind of anchor, moved so it resolves to `x`: a number becomes x, an
// anchor keeps its landmark and changes its offset. Used when a player is
// dragged. The offset is snapped to `step` yards.
export function withX(a: Anchor, x: number, ctx: Ctx, step = 0.5): Anchor {
  if (a.kind === "number") return { kind: "number", value: snap(x, step) };
  const base = resolveX({ ...a, offset: 0 }, ctx);
  const sign = resolveX({ ...a, offset: 1 }, ctx) - base; // +1 or -1
  return { ...a, offset: snap((x - base) / sign, step) };
}

// v to the nearest multiple of `step`, rounded to 0.01 so 0.1 steps don't
// write 2.3000000000000003. (+ 0 turns -0 into 0.)
export const snap = (v: number, step: number) => Math.round(Math.round(v / step) * step * 100) / 100 + 0;

// Places every slot, resolving slot-to-slot anchors on demand. Throws on a
// loop (A anchored to B anchored to A) or a missing slot.
export function placeSlots(
  slots: { name: string; x: Anchor; y: number }[],
  base: { frame: Frame; read?: OffenseRead },
): Record<string, { x: number; y: number }> {
  const placed: Record<string, { x: number; y: number }> = {};
  const busy = new Set<string>();
  const ctx: Ctx = {
    ...base,
    slotPos: (name) => {
      if (placed[name]) return placed[name];
      const slot = slots.find((s) => s.name === name);
      if (!slot) return undefined;
      if (busy.has(name)) throw new Error(`slot '${name}' is anchored to itself through other slots`);
      busy.add(name);
      const pos = { x: resolveX(slot.x, ctx), y: slot.y };
      busy.delete(name);
      placed[name] = pos;
      return pos;
    },
  };
  for (const s of slots) ctx.slotPos(s.name);
  return placed;
}
