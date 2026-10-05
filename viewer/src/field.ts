// Draws an NCAA field: end zones, yard lines every 5, hash marks every yard
// (60 ft from each sideline), sideline ticks, and the yard numbers with their
// direction arrows. Field frame: cfbsim_design.md#field-frame.

import { Camera, FIELD_LENGTH, FIELD_WIDTH } from "./camera";
import type { TeamInfo } from "./tracking";

const HASH_Y = [20, 100 / 3]; // 60 ft from each sideline
const TICK = 2 / 3; // hash marks and sideline ticks are 2 ft long
const NUMBER_Y = 8; // number centers, yd in from each sideline: 6 ft tall, tops 9 yd in
const NUMBER_HEIGHT = 2; // yd

const GRASS = "#2f7a3a";
const GRASS_DARK = "#2a6e34";
const LINE = "rgba(255,255,255,0.85)";

export function drawField(ctx: CanvasRenderingContext2D, cam: Camera, teams: { home: TeamInfo; away: TeamInfo }): void {
  const s = cam.scale;

  // Surround, then alternating 5-yard grass stripes.
  ctx.fillStyle = "#24592c";
  ctx.fillRect(0, 0, cam.width, cam.height);
  for (let x = 10; x < 110; x += 5) {
    rect(ctx, cam, x, 0, x + 5, FIELD_WIDTH, (x / 5) % 2 === 0 ? GRASS : GRASS_DARK);
  }

  // End zones: home on the left, away on the right, with the team abbreviation.
  endZone(ctx, cam, 0, teams.home);
  endZone(ctx, cam, 110, teams.away);

  ctx.strokeStyle = LINE;
  ctx.lineCap = "butt";

  // Sidelines and end lines.
  ctx.lineWidth = Math.max(1.5, s * 0.15);
  const [l, t, w, h] = screenRect(cam, 0, 0, FIELD_LENGTH, FIELD_WIDTH);
  ctx.strokeRect(l, t, w, h);

  // Yard lines every 5 (goal lines are 10 and 110), a little heavier at the goal lines.
  for (let x = 10; x <= 110; x += 5) {
    ctx.lineWidth = Math.max(1, s * (x === 10 || x === 110 ? 0.2 : 0.11));
    line(ctx, cam, x, 0, x, FIELD_WIDTH);
  }

  // One-yard ticks: at both hashes and along both sidelines.
  ctx.lineWidth = Math.max(1, s * 0.1);
  for (let x = 11; x < 110; x++) {
    if (x % 5 === 0) continue;
    for (const hy of HASH_Y) line(ctx, cam, x, hy - TICK / 2, x, hy + TICK / 2);
    line(ctx, cam, x, 0.2, x, 0.2 + TICK);
    line(ctx, cam, x, FIELD_WIDTH - 0.2 - TICK, x, FIELD_WIDTH - 0.2);
  }

  // Numbers on every 10-yard line, read from the near sideline: the top row
  // is upside down. Arrows point toward the nearer goal line.
  if (s >= 4) {
    ctx.fillStyle = LINE;
    ctx.font = `bold ${NUMBER_HEIGHT * s}px system-ui, sans-serif`;
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    for (let x = 20; x <= 100; x += 10) {
      const yards = x <= 60 ? x - 10 : 110 - x;
      for (const [y, flip] of [[NUMBER_Y, false], [FIELD_WIDTH - NUMBER_Y, true]] as const) {
        const [sx, sy] = cam.toScreen(x, y);
        ctx.save();
        ctx.translate(sx, sy);
        ctx.rotate(cam.angle + (flip ? Math.PI : 0));
        ctx.fillText(String(yards), 0, 0);
        ctx.restore();
        if (yards !== 50) arrow(ctx, cam, x, y, x < 60 ? -1 : 1, flip);
      }
    }
  }
}

function endZone(ctx: CanvasRenderingContext2D, cam: Camera, x0: number, team: TeamInfo): void {
  rect(ctx, cam, x0, 0, x0 + 10, FIELD_WIDTH, shade(team.color, 0.55));
  const [sx, sy] = cam.toScreen(x0 + 5, FIELD_WIDTH / 2);
  ctx.save();
  ctx.translate(sx, sy);
  ctx.rotate(cam.angle + (x0 === 0 ? -Math.PI / 2 : Math.PI / 2)); // letters read from the field
  ctx.fillStyle = "rgba(255,255,255,0.8)";
  ctx.font = `bold ${5 * cam.scale}px system-ui, sans-serif`;
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillText(team.abbr, 0, 0);
  ctx.restore();
}

// The small triangle beside a yard number, pointing at the nearer goal line.
function arrow(ctx: CanvasRenderingContext2D, cam: Camera, x: number, y: number, dir: -1 | 1, flip: boolean): void {
  const ax = x + dir * 2.2; // just outside the number
  const ay = y + (flip ? -0.3 : 0.3); // toward the top of the number
  const tip = cam.toScreen(ax + dir * 0.6, ay);
  const b1 = cam.toScreen(ax, ay + 0.3);
  const b2 = cam.toScreen(ax, ay - 0.3);
  ctx.beginPath();
  ctx.moveTo(...tip);
  ctx.lineTo(...b1);
  ctx.lineTo(...b2);
  ctx.closePath();
  ctx.fill();
}

function rect(ctx: CanvasRenderingContext2D, cam: Camera, x0: number, y0: number, x1: number, y1: number, fill: string): void {
  ctx.fillStyle = fill;
  ctx.fillRect(...screenRect(cam, x0, y0, x1, y1));
}

// A field rectangle's [left, top, width, height] on screen, whichever way the
// camera is turned.
function screenRect(cam: Camera, x0: number, y0: number, x1: number, y1: number): [number, number, number, number] {
  const [ax, ay] = cam.toScreen(x0, y0);
  const [bx, by] = cam.toScreen(x1, y1);
  return [Math.min(ax, bx), Math.min(ay, by), Math.abs(bx - ax), Math.abs(by - ay)];
}

function line(ctx: CanvasRenderingContext2D, cam: Camera, x0: number, y0: number, x1: number, y1: number): void {
  ctx.beginPath();
  ctx.moveTo(...cam.toScreen(x0, y0));
  ctx.lineTo(...cam.toScreen(x1, y1));
  ctx.stroke();
}

// Darkens a "#RRGGBB" color by `amount` (0 = unchanged, 1 = black).
export function shade(hex: string, amount: number): string {
  const m = /^#([0-9a-f]{6})$/i.exec(hex);
  if (!m) return hex;
  const n = parseInt(m[1]!, 16);
  const f = (c: number) => Math.round(c * (1 - amount));
  return `rgb(${f(n >> 16)}, ${f((n >> 8) & 255)}, ${f(n & 255)})`;
}
