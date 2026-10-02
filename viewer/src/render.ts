// Draws one moment of a recording on top of the field: overlays, trails,
// players, and the ball.

import { Camera } from "./camera";
import { drawField } from "./field";
import { ballAt, playerAt, type Playback } from "./playback";
import type { Point, Recording, Shape } from "./tracking";

// Built-in layers (computed by the viewer). The file's own overlay layers
// (routes, read, openness, ...) are toggled by name in `hiddenLayers`.
export interface ViewOptions {
  velocity: boolean; // arrow to where each player will be in half a second
  trails: boolean; // the path each player has run so far
  labelMode: "label" | "jersey" | "position" | "none";
  hiddenLayers: Set<string>;
  selected: number | null; // player index
  hovered: number | null;
}

const VELOCITY_LOOKAHEAD = 0.5; // s
const TRAIL_SECONDS = 3;
const MIN_PLAYER_PX = 9; // players never shrink below this on screen, so they stay clickable

export function drawScene(ctx: CanvasRenderingContext2D, cam: Camera, pb: Playback, opts: ViewOptions): void {
  const rec = pb.rec;
  drawField(ctx, cam, rec.header.teams);

  if (opts.trails) drawTrails(ctx, cam, rec, pb.frame, opts.selected);

  // Overlay shapes: the whole-file ones, then this frame's.
  const shapes = [...rec.overlays, ...(rec.debug[pb.wholeFrame] ?? [])];
  for (const shape of shapes) {
    if (!opts.hiddenLayers.has(shape.layer)) drawShape(ctx, cam, pb, shape);
  }

  const players = rec.header.players;
  for (let i = 0; i < players.length; i++) {
    const info = players[i]!;
    const p = pb.player(i);
    const [sx, sy] = cam.toScreen(p.x, p.y);
    const r = playerRadiusPx(cam, info.radius);
    const color = rec.header.teams[info.team].color;

    if (opts.velocity && Math.hypot(p.vx, p.vy) > 0.05) {
      const [ex, ey] = cam.toScreen(p.x + p.vx * VELOCITY_LOOKAHEAD, p.y + p.vy * VELOCITY_LOOKAHEAD);
      arrow(ctx, sx, sy, ex, ey, "rgba(255,255,255,0.9)", Math.max(1.5, cam.scale * 0.08));
    }

    // Body.
    ctx.beginPath();
    ctx.arc(sx, sy, r, 0, Math.PI * 2);
    ctx.fillStyle = color;
    ctx.fill();
    ctx.lineWidth = i === opts.selected ? 3 : 1.5;
    ctx.strokeStyle = i === opts.selected ? "#ffd400" : i === opts.hovered ? "#ffffff" : "rgba(255,255,255,0.75)";
    ctx.stroke();

    // Facing: a notch on the rim (screen angle is negated because screen y is flipped).
    const a = (-p.facingDeg * Math.PI) / 180;
    ctx.beginPath();
    ctx.moveTo(sx + Math.cos(a) * r * 0.45, sy + Math.sin(a) * r * 0.45);
    ctx.lineTo(sx + Math.cos(a) * r * 1.35, sy + Math.sin(a) * r * 1.35);
    ctx.strokeStyle = "#ffffff";
    ctx.lineWidth = Math.max(2, r * 0.25);
    ctx.stroke();

    const text = labelText(rec, i, opts.labelMode);
    if (text) {
      ctx.fillStyle = "#ffffff";
      ctx.textAlign = "center";
      ctx.textBaseline = "middle";
      // Short labels fit inside the circle; longer ones go just below it.
      if (text.length <= 3) {
        ctx.font = `bold ${Math.max(9, r * (text.length <= 2 ? 1.05 : 0.8))}px system-ui, sans-serif`;
        ctx.fillText(text, sx, sy + 0.5);
      } else {
        ctx.font = `bold ${Math.max(10, cam.scale * 0.6)}px system-ui, sans-serif`;
        ctx.fillText(text, sx, sy + r + Math.max(8, cam.scale * 0.5));
      }
    }
  }

  drawBall(ctx, cam, rec, pb.frame);
}

export function playerRadiusPx(cam: Camera, radiusYd: number): number {
  return Math.max(radiusYd * cam.scale, MIN_PLAYER_PX);
}

function labelText(rec: Recording, i: number, mode: ViewOptions["labelMode"]): string {
  const p = rec.header.players[i]!;
  switch (mode) {
    case "label":
      return p.label;
    case "jersey":
      return p.jersey > 0 ? String(p.jersey) : "";
    case "position":
      return p.position;
    case "none":
      return "";
  }
}

// The index of the player under a screen point, or null.
export function playerAtPoint(cam: Camera, pb: Playback, sx: number, sy: number): number | null {
  let best: number | null = null;
  let bestDist = Infinity;
  pb.rec.header.players.forEach((info, i) => {
    const p = pb.player(i);
    const [px, py] = cam.toScreen(p.x, p.y);
    const d = Math.hypot(px - sx, py - sy);
    if (d <= playerRadiusPx(cam, info.radius) + 3 && d < bestDist) {
      best = i;
      bestDist = d;
    }
  });
  return best;
}

function drawTrails(ctx: CanvasRenderingContext2D, cam: Camera, rec: Recording, frame: number, selected: number | null): void {
  // Everyone's last few seconds, or the selected player's whole run.
  const back = Math.round(TRAIL_SECONDS * rec.header.tickRate);
  rec.header.players.forEach((info, i) => {
    const whole = i === selected;
    const start = whole ? 0 : Math.max(0, Math.floor(frame) - back);
    ctx.beginPath();
    for (let f = start; f <= Math.floor(frame); f++) {
      const p = playerAt(rec, i, f);
      const [sx, sy] = cam.toScreen(p.x, p.y);
      if (f === start) ctx.moveTo(sx, sy);
      else ctx.lineTo(sx, sy);
    }
    const now = playerAt(rec, i, frame);
    ctx.lineTo(...cam.toScreen(now.x, now.y));
    ctx.strokeStyle = whole ? "#ffd400" : rec.header.teams[info.team].color;
    ctx.globalAlpha = whole ? 0.9 : 0.55;
    ctx.lineWidth = Math.max(1.5, cam.scale * 0.12);
    ctx.lineJoin = "round";
    ctx.stroke();
    ctx.globalAlpha = 1;
  });
}

// The ball's shadow sits on the ground at (x, y); the ball itself is drawn
// raised by its height z, and a little bigger the higher it is.
function drawBall(ctx: CanvasRenderingContext2D, cam: Camera, rec: Recording, frame: number): void {
  const ball = ballAt(rec, frame);
  if (!ball) return;
  const [x, y, rawZ] = ball;
  const z = Math.max(0, rawZ); // a ball below the ground (e.g. on the tee) draws on it
  const [sx, sy] = cam.toScreen(x, y);
  const size = Math.max(cam.scale * 0.35, 4);

  ctx.beginPath();
  ctx.ellipse(sx, sy, size, size * 0.6, 0, 0, Math.PI * 2);
  ctx.fillStyle = `rgba(0,0,0,${Math.max(0.15, 0.45 - z * 0.02)})`;
  ctx.fill();

  const lift = z * cam.scale * 0.6;
  const grow = 1 + Math.min(z, 25) * 0.03;
  ctx.beginPath();
  ctx.ellipse(sx, sy - lift, size * grow, size * 0.62 * grow, 0, 0, Math.PI * 2);
  ctx.fillStyle = "#8b4a1c";
  ctx.fill();
  ctx.strokeStyle = "#f5f5f5";
  ctx.lineWidth = 1;
  ctx.stroke();
}

function drawShape(ctx: CanvasRenderingContext2D, cam: Camera, pb: Playback, shape: Shape): void {
  const color = shape.color ?? layerColor(shape.layer);
  ctx.strokeStyle = color;
  ctx.fillStyle = color;
  ctx.lineWidth = Math.max(1.5, cam.scale * 0.12);
  ctx.setLineDash(shape.dash ? [6, 5] : []);
  const playerPx = (i: number): Point => {
    const p = pb.player(i);
    return cam.toScreen(p.x, p.y);
  };

  switch (shape.kind) {
    case "line": {
      ctx.beginPath();
      shape.pts.forEach(([x, y], k) => (k === 0 ? ctx.moveTo(...cam.toScreen(x, y)) : ctx.lineTo(...cam.toScreen(x, y))));
      ctx.stroke();
      break;
    }
    case "link": {
      ctx.beginPath();
      ctx.moveTo(...playerPx(shape.a));
      ctx.lineTo(...playerPx(shape.b));
      ctx.lineWidth = Math.max(3, cam.scale * 0.25);
      ctx.stroke();
      break;
    }
    case "ring": {
      const [sx, sy] = playerPx(shape.player);
      const r = playerRadiusPx(cam, pb.rec.header.players[shape.player]!.radius) + 5;
      ctx.beginPath();
      ctx.arc(sx, sy, r, 0, Math.PI * 2);
      ctx.lineWidth = 3;
      ctx.stroke();
      break;
    }
    case "tag": {
      const [sx, sy] = playerPx(shape.player);
      const r = playerRadiusPx(cam, pb.rec.header.players[shape.player]!.radius);
      textBox(ctx, shape.text, sx + r + 4, sy - r - 4, color);
      break;
    }
    case "text": {
      const [sx, sy] = cam.toScreen(...shape.at);
      ctx.font = `bold ${Math.max(11, cam.scale * 0.8)}px system-ui, sans-serif`;
      ctx.textAlign = "center";
      ctx.textBaseline = "middle";
      ctx.fillText(shape.text, sx, sy);
      break;
    }
  }
  ctx.setLineDash([]);
}

function textBox(ctx: CanvasRenderingContext2D, text: string, x: number, y: number, color: string): void {
  ctx.font = "bold 11px system-ui, sans-serif";
  ctx.textAlign = "left";
  ctx.textBaseline = "middle";
  const w = ctx.measureText(text).width + 8;
  ctx.fillStyle = "rgba(0,0,0,0.7)";
  ctx.fillRect(x, y - 8, w, 16);
  ctx.fillStyle = color;
  ctx.fillText(text, x + 4, y);
}

function arrow(ctx: CanvasRenderingContext2D, x0: number, y0: number, x1: number, y1: number, color: string, width: number): void {
  const a = Math.atan2(y1 - y0, x1 - x0);
  const head = Math.max(6, width * 3);
  ctx.strokeStyle = color;
  ctx.fillStyle = color;
  ctx.lineWidth = width;
  ctx.beginPath();
  ctx.moveTo(x0, y0);
  ctx.lineTo(x1 - Math.cos(a) * head * 0.8, y1 - Math.sin(a) * head * 0.8);
  ctx.stroke();
  ctx.beginPath();
  ctx.moveTo(x1, y1);
  ctx.lineTo(x1 - Math.cos(a - 0.4) * head, y1 - Math.sin(a - 0.4) * head);
  ctx.lineTo(x1 - Math.cos(a + 0.4) * head, y1 - Math.sin(a + 0.4) * head);
  ctx.closePath();
  ctx.fill();
}

// A stable color per overlay layer name, for shapes that don't set one.
const LAYER_COLORS: Record<string, string> = {
  routes: "#ffffff",
  read: "#ffd400",
  openness: "#7cfc9a",
  engagements: "#ff5a36",
  targets: "rgba(255,255,255,0.45)",
  marks: "#ffd400",
};
const PALETTE = ["#4fc3f7", "#f48fb1", "#ffb74d", "#aed581", "#ce93d8", "#80cbc4"];

export function layerColor(layer: string): string {
  const known = LAYER_COLORS[layer];
  if (known) return known;
  let h = 0;
  for (const c of layer) h = (h * 31 + c.charCodeAt(0)) >>> 0;
  return PALETTE[h % PALETTE.length]!;
}
