// Reads a tracking file (JSON Lines written by the sim's TrackingWriter) into
// a Recording. Format: docs/design/movement-and-interaction.md#tracking-files.

export interface TeamInfo {
  name: string;
  abbr: string;
  color: string;
}

export interface PlayerInfo {
  id: string;
  name: string;
  label: string;
  team: "home" | "away";
  jersey: number;
  position: string;
  radius: number;
}

export type Point = [number, number];

// Debug overlay shapes (see #debug-overlays in the design doc). `a`, `b`, and
// `player` are indices into the header's players list.
interface ShapeBase {
  layer: string;
  color?: string;
  dash?: boolean;
}
export type Shape = ShapeBase &
  (
    | { kind: "line"; pts: Point[] }
    | { kind: "link"; a: number; b: number }
    | { kind: "ring"; player: number }
    | { kind: "tag"; player: number; text: string }
    | { kind: "text"; at: Point; text: string }
  );

export interface TrackingEvent {
  tick: number;
  frame: number; // index of the first frame at or after `tick`
  event: string;
  fields: Record<string, unknown>; // everything else on the line
}

export interface Header {
  formatVersion: number;
  simVersion: string;
  kind: string;
  name: string;
  seed: number;
  tickRate: number;
  teams: { home: TeamInfo; away: TeamInfo };
  details: Record<string, unknown>;
  players: PlayerInfo[];
}

// Values stored per player per frame, in this order.
export const X = 0;
export const Y = 1;
export const VX = 2;
export const VY = 3;
export const FACING = 4;
export const STRIDE = 5;

export interface Recording {
  header: Header;
  frameCount: number;
  ticks: Int32Array; // the sim tick number of each frame
  // Player state, frame-major: player i of frame f starts at (f * players + i) * STRIDE.
  // A typed array keeps a full game's worth of frames compact and fast to read.
  state: Float32Array;
  ball: (Float32Array | null)[]; // [x, y, z] per frame, or null when there's no live ball
  overlays: Shape[]; // shown for the whole recording
  debug: (Shape[] | undefined)[]; // per frame
  events: TrackingEvent[];
  layers: string[]; // every overlay layer name used in the file, sorted
}

export class TrackingError extends Error {}

const KINDS = new Set(["line", "link", "ring", "tag", "text"]);

export function parseTracking(text: string): Recording {
  const lines = text.split("\n");
  let header: Header | null = null;
  const tickNumbers: number[] = [];
  const stateChunks: Float32Array[] = [];
  const ball: (Float32Array | null)[] = [];
  const debug: (Shape[] | undefined)[] = [];
  const rawEvents: Omit<TrackingEvent, "frame">[] = [];
  const layers = new Set<string>();
  let overlays: Shape[] = [];

  for (let n = 0; n < lines.length; n++) {
    const raw = lines[n]!.trim();
    if (raw === "") continue;
    const where = `line ${n + 1}`;
    let j: any;
    try {
      j = JSON.parse(raw);
    } catch (e) {
      throw new TrackingError(`${where}: not valid JSON (${(e as Error).message})`);
    }

    if (header === null) {
      if (j.type !== "header") throw new TrackingError(`${where}: expected the header line first`);
      header = parseHeader(j, where);
      overlays = parseShapes(j.overlays, `${where}: overlays`, header.players.length, layers);
      continue;
    }

    switch (j.type) {
      case "tick": {
        const tick = need(j.tick, "number", `${where}: tick`);
        const prev = tickNumbers[tickNumbers.length - 1];
        if (prev !== undefined && tick <= prev) {
          throw new TrackingError(`${where}: tick ${tick} is not after tick ${prev}`);
        }
        tickNumbers.push(tick);
        stateChunks.push(parsePlayers(j.p, header.players.length, where));
        ball.push(parseBall(j.ball, where));
        const shapes = parseShapes(j.dbg, `${where}: dbg`, header.players.length, layers);
        debug.push(shapes.length > 0 ? shapes : undefined);
        break;
      }
      case "event": {
        const { type: _type, tick, event, ...fields } = j;
        rawEvents.push({
          tick: need(tick, "number", `${where}: tick`),
          event: need(event, "string", `${where}: event`),
          fields,
        });
        break;
      }
      default:
        // Unknown line types are skipped, so newer sims still load in older viewers.
        break;
    }
  }

  if (header === null) throw new TrackingError("the file is empty");
  if (tickNumbers.length === 0) throw new TrackingError("the file has no tick lines");

  const perFrame = header.players.length * STRIDE;
  const state = new Float32Array(tickNumbers.length * perFrame);
  stateChunks.forEach((chunk, f) => state.set(chunk, f * perFrame));
  const ticks = Int32Array.from(tickNumbers);

  return {
    header,
    frameCount: tickNumbers.length,
    ticks,
    state,
    ball,
    overlays,
    debug,
    events: rawEvents.map((e) => ({ ...e, frame: frameAtTick(ticks, e.tick) })),
    layers: [...layers].sort(),
  };
}

// The first frame whose tick is >= `tick` (clamped to the last frame).
export function frameAtTick(ticks: Int32Array, tick: number): number {
  let lo = 0;
  let hi = ticks.length - 1;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (ticks[mid]! < tick) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

function parseHeader(j: any, where: string): Header {
  const players = need(j.players, "array", `${where}: players`) as any[];
  return {
    formatVersion: need(j.format_version, "number", `${where}: format_version`),
    simVersion: String(j.sim_version ?? "?"),
    kind: String(j.kind ?? "?"),
    name: String(j.name ?? ""),
    seed: Number(j.seed ?? 0),
    tickRate: need(j.tick_rate, "number", `${where}: tick_rate`),
    teams: {
      home: parseTeam(j.teams?.home, "Home", "#C8102E"),
      away: parseTeam(j.teams?.away, "Away", "#1D4F91"),
    },
    details: j.details ?? {},
    players: players.map((p, i) => {
      const at = `${where}: players[${i}]`;
      const team = need(p.team, "string", `${at}.team`);
      if (team !== "home" && team !== "away") {
        throw new TrackingError(`${at}.team: expected "home" or "away", got "${team}"`);
      }
      return {
        id: need(p.id, "string", `${at}.id`),
        name: String(p.name ?? p.id),
        label: String(p.label ?? ""),
        team,
        jersey: Number(p.jersey ?? 0),
        position: String(p.position ?? ""),
        radius: Number(p.radius ?? 0.4),
      };
    }),
  };
}

function parseTeam(j: any, name: string, color: string): TeamInfo {
  return { name: String(j?.name ?? name), abbr: String(j?.abbr ?? name.toUpperCase()), color: String(j?.color ?? color) };
}

function parsePlayers(p: unknown, count: number, where: string): Float32Array {
  if (!Array.isArray(p) || p.length !== count) {
    throw new TrackingError(`${where}: p: expected ${count} players (one per header entry)`);
  }
  const out = new Float32Array(count * STRIDE);
  p.forEach((values, i) => {
    if (!Array.isArray(values) || values.length !== STRIDE) {
      throw new TrackingError(`${where}: p[${i}]: expected [x, y, vx, vy, facing_deg]`);
    }
    for (let k = 0; k < STRIDE; k++) out[i * STRIDE + k] = Number(values[k]);
  });
  return out;
}

function parseBall(b: unknown, where: string): Float32Array | null {
  if (b === null || b === undefined) return null;
  if (!Array.isArray(b) || b.length !== 3) throw new TrackingError(`${where}: ball: expected [x, y, z] or null`);
  return Float32Array.from(b.map(Number));
}

function parseShapes(list: unknown, where: string, players: number, layers: Set<string>): Shape[] {
  if (list === undefined) return [];
  if (!Array.isArray(list)) throw new TrackingError(`${where}: expected an array of shapes`);
  return list.map((s: any, i) => {
    const at = `${where}[${i}]`;
    const kind = need(s?.kind, "string", `${at}.kind`);
    if (!KINDS.has(kind)) {
      throw new TrackingError(`${at}.kind: unknown shape '${kind}' (expected one of: ${[...KINDS].join(", ")})`);
    }
    layers.add(need(s.layer, "string", `${at}.layer`));
    for (const key of ["a", "b", "player"]) {
      if (key in s && !(Number.isInteger(s[key]) && s[key] >= 0 && s[key] < players)) {
        throw new TrackingError(`${at}.${key}: no player with index ${s[key]}`);
      }
    }
    return s as Shape;
  });
}

function need(value: any, type: "number" | "string" | "array", what: string): any {
  const ok = type === "array" ? Array.isArray(value) : typeof value === type;
  if (!ok) throw new TrackingError(`${what}: expected a ${type}`);
  return value;
}
