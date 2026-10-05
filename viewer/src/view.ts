// The vertical view: which way is up, and where the camera sits.
//
// "Behind" a team means that team attacks up the screen. That one rule puts
// you behind your offense when you have the ball and behind your defense when
// you don't (the other team comes down the screen at you), and the view turns
// over when the teams change ends.

import type { Turns } from "./camera";
import type { TeamKey } from "./game";
import { ballAt, playerAt } from "./playback";
import type { Recording } from "./tracking";

export type ViewMode = "horizontal" | "vertical";
export type Behind = "offense" | TeamKey;

// The team with the ball at the snap: the file's `offense` (between plays,
// and a kick's kicking team); older files only by agent 0, the play sim's
// offense (on a kick, the receiving team).
export function offenseOf(rec: Recording): TeamKey | undefined {
  const o = rec.header.details.offense;
  return o === "home" || o === "away" ? o : rec.header.players[0]?.team;
}

// Whether `side` attacks +x on this snap, or null if the file doesn't say.
// Game, play, and between-plays files carry `home_right`; older play files
// only the offense's `direction` (agent 0 is always on the offense).
export function attacksRight(rec: Recording, side: TeamKey): boolean | null {
  const d = rec.header.details;
  if (typeof d.home_right === "boolean") return side === "home" ? d.home_right : !d.home_right;
  const offense = rec.header.players[0]?.team;
  if (offense && (d.direction === "right" || d.direction === "left")) {
    return (d.direction === "right") === (side === offense);
  }
  return null;
}

// The camera's quarter turns for a view. Vertical puts the `behind` team's
// attack up the screen; with nothing to go on (a drill), +x is up.
export function viewTurns(rec: Recording, mode: ViewMode, behind: Behind): Turns {
  if (mode === "horizontal") return 0;
  const side = behind === "offense" ? (offenseOf(rec) ?? "home") : behind;
  return attacksRight(rec, side) === false ? 3 : 1;
}

// The field x the vertical camera centers on at `frame`: the ball, or, in a
// file without one (a drill), the middle of the players.
export function followX(rec: Recording, frame: number): number | null {
  const ball = ballAt(rec, frame);
  if (ball) return ball[0];
  const spot = rec.header.details.spot;
  if (Array.isArray(spot) && spot.length === 2) return Number(spot[0]);
  const n = rec.header.players.length;
  if (n === 0) return null;
  let sum = 0;
  for (let i = 0; i < n; i++) sum += playerAt(rec, i, frame).x;
  return sum / n;
}

// The field point a fixed-zoom camera follows at `frame`: the ball; between
// plays before it's spotted, its spot; or the middle of the players in a file
// without one (a drill).
export function followPoint(rec: Recording, frame: number): [number, number] | null {
  const ball = ballAt(rec, frame);
  if (ball) return [ball[0], ball[1]];
  const spot = rec.header.details.spot;
  if (Array.isArray(spot) && spot.length === 2) return [Number(spot[0]), Number(spot[1])];
  const n = rec.header.players.length;
  if (n === 0) return null;
  let sx = 0;
  let sy = 0;
  for (let i = 0; i < n; i++) {
    const p = playerAt(rec, i, frame);
    sx += p.x;
    sy += p.y;
  }
  return [sx / n, sy / n];
}

// The vertical view looks this many yards up the screen past the ball (the
// way the offense is going): the space behind the play is mostly empty.
export const VERTICAL_LEAD = 10;

// The camera eases toward the point it follows (time constant, s of play),
// so the snap (the ball going back to the QB) and a pitch don't jerk it; it
// jumps when it's further than CAMERA_JUMP yards off (a new file somewhere
// else on the field).
export const CAMERA_EASE = 0.35;
export const CAMERA_JUMP = 15;

// The eased point: `from` moved toward `to` over `dt` s (no `from`, or too
// far off: straight there).
export function easeToward(from: [number, number] | null, to: [number, number], dt: number): [number, number] {
  if (!from || Math.hypot(to[0] - from[0], to[1] - from[1]) > CAMERA_JUMP) return [to[0], to[1]];
  const k = 1 - Math.exp(-Math.max(dt, 0) / CAMERA_EASE);
  return [from[0] + (to[0] - from[0]) * k, from[1] + (to[1] - from[1]) * k];
}

// Fixed zoom's default, in yards of the field's width on screen: sideline to
// sideline with a little room, which is what fitting a normal 5-10 yard play
// shows (every player in view). Saved separately per view once zoomed.
export const DEFAULT_YARDS: Record<ViewMode, number> = { horizontal: 160 / 3 + 6, vertical: 160 / 3 + 6 };
