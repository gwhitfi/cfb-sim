// The playback clock and reading player state at any (fractional) frame.
//
// Positions are recorded at 20 Hz but the screen redraws at ~60 Hz, so between
// two recorded frames we interpolate linearly. Time is kept as a fractional
// frame index rather than seconds: frames are what the file has, and gaps in
// the tick numbers (dead time skipped in a game) just play straight through.

import { FACING, STRIDE, X, Y, VX, VY, Z, type Recording } from "./tracking";

export interface PlayerState {
  x: number;
  y: number;
  vx: number;
  vy: number;
  facingDeg: number;
  z: number; // feet above the ground (a jump)
}

export const SPEEDS = [0.1, 0.25, 0.5, 1, 2, 4] as const;

// The events that end a play: the sim's whistle outcomes (run_play) and kick
// results (run_kick). Anything else (a tackle try, a catch) happens during it.
export const DEAD_BALL_EVENTS = new Set([
  // plays
  "tackled", "sack", "safety", "touchdown", "out_of_bounds", "incomplete", "down",
  "fumble_out_of_bounds", "recovered", "whistle",
  // kicks
  "returned", "touchback", "fair_catch", "downed", "made", "missed", "blocked",
  "recovered_kicking", "recovered_receiving",
]);

// The frames from the snap to the whistle (dev mode), or null for a recording
// with no snap (a drill). A play with no dead-ball event runs to the end.
export function snapToWhistle(rec: Recording): [number, number] | null {
  const snap = rec.events.find((e) => e.event === "snap");
  if (!snap) return null;
  const end = rec.events.find((e) => e.frame >= snap.frame && e.event !== "snap" && DEAD_BALL_EVENTS.has(e.event));
  return [snap.frame, end ? Math.max(end.frame, snap.frame) : rec.frameCount - 1];
}

export class Playback {
  frame = 0; // fractional
  playing = false;
  speed = 1;

  // Plays only these frames (first, last), e.g. snap to whistle; null: all.
  private range: [number, number] | null = null;

  constructor(public readonly rec: Recording) {}

  get firstFrame(): number {
    return this.range ? this.range[0] : 0;
  }

  get lastFrame(): number {
    return this.range ? this.range[1] : this.rec.frameCount - 1;
  }

  // Limits playback to frames [first, last] (null: the whole recording) and
  // keeps the clock inside them.
  setRange(range: [number, number] | null): void {
    this.range = range;
    this.seek(this.frame);
  }

  get atEnd(): boolean {
    return this.frame >= this.lastFrame;
  }

  // Advances the clock by `seconds` of wall time. Stops at the end.
  advance(seconds: number): void {
    if (!this.playing) return;
    this.seek(this.frame + seconds * this.speed * this.rec.header.tickRate);
    if (this.atEnd) this.playing = false;
  }

  seek(frame: number): void {
    this.frame = Math.min(Math.max(frame, this.firstFrame), this.lastFrame);
  }

  // One recorded frame forward or back, landing exactly on a frame.
  step(direction: 1 | -1): void {
    this.playing = false;
    const f = direction > 0 ? Math.floor(this.frame + 1e-6) + 1 : Math.ceil(this.frame - 1e-6) - 1;
    this.seek(f);
  }

  togglePlay(): void {
    if (!this.playing && this.atEnd) this.frame = this.firstFrame; // play again from the start
    this.playing = !this.playing;
  }

  // The recorded frame we're on or just past.
  get wholeFrame(): number {
    return Math.floor(this.frame + 1e-6);
  }

  get tick(): number {
    return this.rec.ticks[this.wholeFrame]!;
  }

  // Seconds since the first frame, from tick numbers.
  get seconds(): number {
    const f = this.wholeFrame;
    const next = Math.min(f + 1, this.lastFrame);
    const t0 = this.rec.ticks[f]!;
    const t = t0 + (this.rec.ticks[next]! - t0) * (this.frame - f);
    return (t - this.rec.ticks[0]!) / this.rec.header.tickRate;
  }

  player(index: number): PlayerState {
    return playerAt(this.rec, index, this.frame);
  }
}

export function playerAt(rec: Recording, index: number, frame: number): PlayerState {
  const f0 = Math.min(Math.max(Math.floor(frame), 0), rec.frameCount - 1);
  const f1 = Math.min(f0 + 1, rec.frameCount - 1);
  const t = Math.min(Math.max(frame - f0, 0), 1);
  const n = rec.header.players.length;
  const a = (f0 * n + index) * STRIDE;
  const b = (f1 * n + index) * STRIDE;
  const s = rec.state;
  const lerp = (k: number) => s[a + k]! + (s[b + k]! - s[a + k]!) * t;
  return {
    x: lerp(X),
    y: lerp(Y),
    vx: lerp(VX),
    vy: lerp(VY),
    facingDeg: lerpAngleDeg(s[a + FACING]!, s[b + FACING]!, t),
    z: lerp(Z),
  };
}

// Interpolates between two angles the short way round (170° → -170° passes
// through 180°, not through 0°).
export function lerpAngleDeg(a: number, b: number, t: number): number {
  let d = (b - a) % 360;
  if (d > 180) d -= 360;
  if (d < -180) d += 360;
  return a + d * t;
}

export function ballAt(rec: Recording, frame: number): [number, number, number] | null {
  const f0 = Math.min(Math.max(Math.floor(frame), 0), rec.frameCount - 1);
  const f1 = Math.min(f0 + 1, rec.frameCount - 1);
  const a = rec.ball[f0];
  if (!a) return null;
  const b = rec.ball[f1] ?? a; // ball goes dead next frame: hold it in place
  const t = Math.min(Math.max(frame - f0, 0), 1);
  return [a[0]! + (b[0]! - a[0]!) * t, a[1]! + (b[1]! - a[1]!) * t, a[2]! + (b[2]! - a[2]!) * t];
}

// The way the ball is pointing at `frame`, as a unit vector on the ground:
// its direction of travel (in the air, the pass's line; held, where the
// carrier is going). A ball that isn't moving keeps the way it last moved;
// null if it hasn't moved yet (the viewer then points it along +x).
export function ballHeading(rec: Recording, frame: number): [number, number] | null {
  const MOVING = 0.01; // yd per frame
  for (let f = Math.min(Math.max(Math.floor(frame), 0), rec.frameCount - 1); f >= 0; f--) {
    const a = rec.ball[f];
    const b = rec.ball[f + 1];
    if (!a || !b) continue;
    const dx = b[0]! - a[0]!;
    const dy = b[1]! - a[1]!;
    const len = Math.hypot(dx, dy);
    if (len > MOVING) return [dx / len, dy / len];
  }
  return null;
}
