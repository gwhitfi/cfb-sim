// Wires the page together: loading files, the playback loop, mouse and
// keyboard controls, and the side panel.

import { Camera } from "./camera";
import { DEAD_BALL_EVENTS, Playback, snapToWhistle, SPEEDS } from "./playback";
import { drawScene, layerColor, playerAtPoint, type ViewOptions } from "./render";
import { describeEvent, eventDetails, eventTime, text as valueText } from "./events";
import { buildCommentary, lineAt, teamColors, type Line } from "./commentary";
import { parseTracking, TrackingError, X, Y, STRIDE, type Recording } from "./tracking";
import { Live } from "./live";
import { TopBar } from "./topbar";
import type { Defaults } from "./settings";
import { DEFAULT_YARDS, VERTICAL_LEAD, easeToward, followPoint, viewTurns, type Behind, type ViewMode } from "./view";

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;

const canvas = $<HTMLCanvasElement>("canvas");
const ctx = canvas.getContext("2d")!;
const stage = $<HTMLDivElement>("stage");
const scrub = $<HTMLInputElement>("scrub");
const playButton = $<HTMLButtonElement>("play");
const speedSelect = $<HTMLSelectElement>("speed");
const reloadButton = $<HTMLButtonElement>("reload");
const tooltip = $<HTMLDivElement>("tooltip");
const viewSelect = $<HTMLSelectElement>("view");
const behindSelect = $<HTMLSelectElement>("view-behind");
const zoomSelect = $<HTMLSelectElement>("zoom");

// Fixed zoom: yards across the screen in each view (saved), and how far the
// user has dragged the view off the ball on this play (not saved).
const zoomYards: Record<ViewMode, number> = { ...DEFAULT_YARDS };
let panOffset: [number, number] = [0, 0];
// The point the camera follows, eased (see easeToward); null: none yet.
let follow: [number, number] | null = null;
let followFrame = -1; // the frame it was last moved at

const PLAY_ICON = `<svg viewBox="0 0 16 16" width="14" height="14"><path d="M4 2v12l10-6z" fill="currentColor"/></svg>`;
const PAUSE_ICON = `<svg viewBox="0 0 16 16" width="14" height="14"><path d="M3 2h4v12H3zM9 2h4v12H9z" fill="currentColor"/></svg>`;
playButton.innerHTML = PLAY_ICON;

const cam = new Camera();
let pb: Playback | null = null;
let source: { name: string; repoPath?: string } | null = null;

const opts: ViewOptions = {
  velocity: false,
  trails: false,
  names: true,
  labelMode: "label",
  hiddenLayers: new Set(),
  selected: null,
  hovered: null,
};

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

function load(text: string, name: string, repoPath?: string): boolean {
  let rec: Recording;
  try {
    rec = parseTracking(text);
  } catch (e) {
    showError(e instanceof TrackingError ? `${name}: ${e.message}` : String(e));
    return false;
  }
  // Reloading the same file keeps the view and the moment you were looking at.
  const sameFile = source?.name === name && pb !== null;
  const keepFrame = sameFile ? pb!.frame : 0;
  source = { name, repoPath };
  pb = new Playback(rec);
  pb.speed = Number(speedSelect.value);
  applyDevMode();
  pb.seek(keepFrame);
  if (!sameFile) panOffset = [0, 0];
  updateFollow(0); // (eases on from the last file when it's nearby: between plays into the snap)
  if (!sameFile) {
    opts.selected = null;
    opts.hiddenLayers.clear();
  }
  applyView(!sameFile);
  if (repoPath) history.replaceState(null, "", `#file=${encodeURIComponent(repoPath)}`);

  $("empty").hidden = true;
  $("error").hidden = true;
  reloadButton.disabled = !repoPath;
  buildInfo(rec);
  buildLayers(rec);
  buildEvents(rec);
  commentary = buildCommentary(rec);
  shownLine = null;
  updatePanel();
  return true;
}

// Live games and full-game playback (live.ts): each snap is loaded like a file.
const live = new Live({
  show(text, name) {
    if (!load(text, name)) return false;
    pb!.speed = Number(speedSelect.value);
    pb!.playing = true;
    return true;
  },
  playback: () => pb,
  devMode: () => devBox.checked,
  serverInfo: () => topBar.serverInfo(),
});

// The top bar's New Game, Replays, and Settings (topbar.ts).
const topBar = new TopBar({
  live,
  speeds: SPEEDS,
  gameView(view, behind, speed) {
    speedSelect.value = String(speed);
    if (pb) pb.speed = speed;
    viewSelect.value = view;
    behindSelect.value = behind;
    behindSelect.hidden = view !== "vertical";
    saveView();
    applyView(true);
  },
  trackingFiles,
  currentFile: () => source?.repoPath,
  openRepoFile: (path) => void loadRepoFile(path),
  openFile: (file) => void loadFile(file),
  defaultsChanged(d) {
    applyDefaultView(d);
    openSections(d);
  },
});

function showError(message: string): void {
  const el = $("error");
  el.textContent = message;
  el.hidden = false;
}

async function loadFile(file: File): Promise<void> {
  load(await file.text(), file.name);
}

// The dev server (vite.config.ts) lists and serves tracking files from the
// repo (Replays → Play), so you can rerun the CLI and hit Reload. A
// production build has no such endpoint: null.
async function trackingFiles(): Promise<string[] | null> {
  try {
    const res = await fetch("/api/tracking");
    if (!res.ok || !res.headers.get("content-type")?.includes("json")) return null;
    return (await res.json()) as string[];
  } catch {
    return null;
  }
}

async function loadRepoFile(path: string): Promise<void> {
  const res = await fetch(`/api/tracking/file?path=${encodeURIComponent(path)}`);
  if (!res.ok) {
    showError(`${path}: ${res.status} ${await res.text()}`);
    return;
  }
  load(await res.text(), path.split("/").pop()!, path);
}

reloadButton.addEventListener("click", () => {
  if (source?.repoPath) void loadRepoFile(source.repoPath);
});

// Drag and drop anywhere on the page.
let dragDepth = 0;
window.addEventListener("dragenter", (e) => {
  e.preventDefault();
  dragDepth++;
  stage.classList.add("dragging");
});
window.addEventListener("dragleave", () => {
  if (--dragDepth === 0) stage.classList.remove("dragging");
});
window.addEventListener("dragover", (e) => e.preventDefault());
window.addEventListener("drop", (e) => {
  e.preventDefault();
  dragDepth = 0;
  stage.classList.remove("dragging");
  const file = e.dataTransfer?.files[0];
  if (file) void loadFile(file);
});

// ---------------------------------------------------------------------------
// Side panel
// ---------------------------------------------------------------------------

function buildInfo(rec: Recording): void {
  const h = rec.header;
  const rows: [string, string][] = [
    ["Kind", `${h.kind} · ${h.name}`],
    ["Seed", String(h.seed)],
    ["Sim", `v${h.simVersion} (format ${h.formatVersion})`],
    ["Players", `${h.players.length}`],
    ["Length", `${((rec.ticks[rec.frameCount - 1]! - rec.ticks[0]!) / h.tickRate).toFixed(2)} s · ${rec.frameCount} frames`],
  ];
  for (const [k, v] of Object.entries(h.details)) rows.push([k, valueText(v)]);
  $("info").replaceChildren(
    ...rows.flatMap(([k, v]) => {
      const dt = document.createElement("dt");
      dt.textContent = k;
      const dd = document.createElement("dd");
      dd.textContent = v;
      return [dt, dd];
    }),
  );
}

function buildLayers(rec: Recording): void {
  $("layers").replaceChildren(
    ...rec.layers.map((layer) => {
      const label = document.createElement("label");
      const box = document.createElement("input");
      box.type = "checkbox";
      box.checked = !opts.hiddenLayers.has(layer);
      box.addEventListener("change", () => {
        if (box.checked) opts.hiddenLayers.delete(layer);
        else opts.hiddenLayers.add(layer);
      });
      const swatch = document.createElement("span");
      swatch.className = "swatch";
      swatch.style.background = layerColor(layer);
      label.append(box, swatch, layer);
      return label;
    }),
  );
}

// The events that matter most to how a play went, highlighted in the list.
const KEY_EVENTS = new Set([
  ...DEAD_BALL_EVENTS, "snap", "handoff", "throw", "catch", "interception", "fumble", "tackle",
  "broken_tackle", "missed_tackle", "drag_down", "bust",
]);

const detailBox = $<HTMLInputElement>("events-detail");

// ---------------------------------------------------------------------------
// Highlights: the play's key moments as commentary along the bottom of the
// field (commentary.ts), in the colors of the team with the ball
// ---------------------------------------------------------------------------

const highlightsBox = $<HTMLInputElement>("opt-highlights");
const highlights = $<HTMLDivElement>("highlights");
let commentary: Line[] = [];
let shownLine: string | null = null; // what's on the bar now ("" = hidden)

function updateHighlights(): void {
  const i = pb && highlightsBox.checked ? lineAt(commentary, pb.wholeFrame) : -1;
  const line = i >= 0 ? commentary[i]! : null;
  const key = line ? `${i}` : "";
  if (key === shownLine) return;
  shownLine = key;
  highlights.hidden = !line;
  if (!line || !pb) return;
  const team = pb.rec.header.teams[line.team];
  const c = teamColors(team.color);
  highlights.style.setProperty("--team", c.base);
  highlights.style.setProperty("--team-dark", c.dark);
  highlights.style.setProperty("--team-ink", c.ink);
  $("hl-badge").textContent = team.abbr;
  // The line before it stays above, smaller, while it's the same play.
  const prev = i > 0 ? commentary[i - 1]! : null;
  $("hl-prev").textContent = prev?.text ?? "";
  const el = $("hl-line");
  el.textContent = line.text;
  el.classList.toggle("big", !!line.big);
  el.classList.toggle("final", !!line.final);
  // Restart the entrance animation for the new line.
  el.classList.remove("enter");
  void el.offsetWidth;
  el.classList.add("enter");
}

highlightsBox.addEventListener("change", () => {
  shownLine = null;
  updateHighlights();
  saveView();
  highlightsBox.blur();
});

// The event list: times from the snap, each event's line, and (Details) every
// field it carries, with player names next to slots.
function buildEvents(rec: Recording): void {
  const list = $("events");
  const names = new Map(rec.header.players.map((p) => [p.label, p.name]));
  const snap = rec.events.find((e) => e.event === "snap");
  const detail = detailBox.checked;
  list.replaceChildren(
    ...rec.events.map((ev) => {
      const li = document.createElement("li");
      if (KEY_EVENTS.has(ev.event)) li.classList.add("key-event");
      const t = document.createElement("span");
      t.className = "t";
      t.textContent = eventTime(ev, snap?.tick ?? null, rec.ticks[0]!, rec.header.tickRate);
      const what = document.createElement("div");
      what.className = "what";
      const text = document.createElement("span");
      text.textContent = describeEvent(ev);
      what.append(text);
      const fields = detail ? eventDetails(ev, names) : [];
      if (fields.length) {
        const d = document.createElement("div");
        d.className = "detail";
        for (const [k, v] of fields) {
          const item = document.createElement("span");
          const key = document.createElement("b");
          key.textContent = k;
          item.append(key, v);
          d.append(item);
        }
        what.append(d);
      }
      li.append(t, what);
      li.addEventListener("click", () => {
        pb?.seek(ev.frame);
        if (pb) pb.playing = false;
      });
      return li;
    }),
  );
  if (rec.events.length === 0) list.innerHTML = `<li class="muted">None</li>`;
}

let lastPanelFrame = -1;

// Updates the parts of the panel and transport that change during playback.
function updatePanel(): void {
  if (!pb) return;
  const rec = pb.rec;
  updateHighlights();
  if (playButton.dataset.playing !== String(pb.playing)) {
    playButton.dataset.playing = String(pb.playing);
    playButton.innerHTML = pb.playing ? PAUSE_ICON : PLAY_ICON;
  }
  scrub.value = String(pb.wholeFrame);
  $("clock").textContent = `${pb.seconds.toFixed(2)} s · tick ${pb.tick}`;

  if (pb.wholeFrame !== lastPanelFrame) {
    lastPanelFrame = pb.wholeFrame;
    rec.events.forEach((ev, i) => {
      const li = $("events").children[i];
      li?.classList.toggle("past", ev.frame < pb!.wholeFrame);
      li?.classList.toggle("current", ev.frame === pb!.wholeFrame);
    });
  }

  const sel = opts.selected;
  const el = $("player");
  if (sel === null) {
    el.className = "muted";
    el.textContent = "Click a player";
  } else {
    const info = rec.header.players[sel]!;
    const p = pb.player(sel);
    const speed = Math.hypot(p.vx, p.vy);
    el.className = "";
    el.innerHTML = "";
    const rows: [string, string][] = [
      ["Name", info.name],
      ["Id", info.id],
      ["Team", `${rec.header.teams[info.team].abbr} · ${info.position}${info.jersey ? ` #${info.jersey}` : ""}`],
      ["Position", `(${p.x.toFixed(2)}, ${p.y.toFixed(2)}) yd`],
      ["Speed", `${speed.toFixed(2)} yd/s · ${(speed * 2.0455).toFixed(1)} mph`],
      ["Facing", `${p.facingDeg.toFixed(0)}°`],
    ];
    const dl = document.createElement("dl");
    dl.className = "info";
    for (const [k, v] of rows) {
      const dt = document.createElement("dt");
      dt.textContent = k;
      const dd = document.createElement("dd");
      dd.textContent = v;
      dl.append(dt, dd);
    }
    el.append(dl);
  }
}

// ---------------------------------------------------------------------------
// Transport and keyboard
// ---------------------------------------------------------------------------

for (const s of SPEEDS) speedSelect.append(new Option(`${s}×`, String(s)));
speedSelect.value = "1";
speedSelect.addEventListener("change", () => {
  if (pb) pb.speed = Number(speedSelect.value);
  saveView();
  speedSelect.blur(); // so the keyboard shortcuts keep working
});

function changeSpeed(dir: 1 | -1): void {
  const i = SPEEDS.indexOf(Number(speedSelect.value) as (typeof SPEEDS)[number]);
  const next = SPEEDS[Math.min(Math.max(i + dir, 0), SPEEDS.length - 1)]!;
  speedSelect.value = String(next);
  if (pb) pb.speed = next;
  saveView();
}

playButton.addEventListener("click", () => pb?.togglePlay());
$("back").addEventListener("click", () => pb?.step(-1));
$("fwd").addEventListener("click", () => pb?.step(1));
scrub.addEventListener("input", () => {
  if (!pb) return;
  pb.playing = false;
  pb.seek(Number(scrub.value));
});
$("fit").addEventListener("click", (e) => {
  if (!e.shiftKey) fitView();
  else if (fixedZoom()) panOffset = [0, 0];
  else fitPlayers();
});

detailBox.addEventListener("change", () => {
  if (pb) buildEvents(pb.rec);
  lastPanelFrame = -1; // re-mark the past and current events
  saveView();
  detailBox.blur();
});

// The side panel's sections start open or folded as the Defaults say.
function openSections(d: Defaults): void {
  for (const [id, open] of Object.entries(d.open)) {
    const el = document.getElementById(id);
    if (el instanceof HTMLDetailsElement) el.open = open;
  }
}
openSections(topBar.defaults);

// Dev mode: play only from the snap to the whistle (no huddle, no pull-up).
const devBox = $<HTMLInputElement>("dev-mode");

function applyDevMode(): void {
  if (!pb) return;
  pb.setRange(devBox.checked ? snapToWhistle(pb.rec) : null);
  scrub.min = String(pb.firstFrame);
  scrub.max = String(pb.lastFrame);
}

devBox.addEventListener("change", () => {
  applyDevMode();
  saveView();
  devBox.blur(); // so the keyboard shortcuts keep working
});

const velocityBox = $<HTMLInputElement>("opt-velocity");
const trailsBox = $<HTMLInputElement>("opt-trails");
const namesBox = $<HTMLInputElement>("opt-names");
const labelsSelect = $<HTMLSelectElement>("opt-labels");
velocityBox.addEventListener("change", () => (opts.velocity = velocityBox.checked));
trailsBox.addEventListener("change", () => (opts.trails = trailsBox.checked));
namesBox.checked = opts.names;
namesBox.addEventListener("change", () => (opts.names = namesBox.checked));
labelsSelect.addEventListener("change", () => (opts.labelMode = labelsSelect.value as ViewOptions["labelMode"]));

window.addEventListener("keydown", (e) => {
  if (e.target instanceof HTMLInputElement && e.target.type !== "range" && e.target.type !== "checkbox") return;
  if (e.target instanceof HTMLSelectElement) return;
  if (live.key(e)) {
    e.preventDefault();
    return;
  }
  const key = e.key;
  let handled = true;
  if (key === " ") pb?.togglePlay();
  else if (key === "ArrowRight") e.shiftKey ? pb?.seek(pb.frame + pb.rec.header.tickRate) : pb?.step(1);
  else if (key === "ArrowLeft") e.shiftKey ? pb?.seek(pb.frame - pb.rec.header.tickRate) : pb?.step(-1);
  else if (key === "Home") pb?.seek(pb.firstFrame);
  else if (key === "End") pb?.seek(pb.lastFrame);
  else if (key === "]") changeSpeed(1);
  else if (key === "[") changeSpeed(-1);
  else if (key === "f") fitView();
  else if (key === "c") {
    viewSelect.value = viewSelect.value === "vertical" ? "horizontal" : "vertical";
    viewSelect.dispatchEvent(new Event("change"));
  }
  else if (key === "F") fixedZoom() ? (panOffset = [0, 0]) : fitPlayers();
  else if (key === "v") velocityBox.click();
  else if (key === "t") trailsBox.click();
  else if (key === "n") namesBox.click();
  else if (key === "r") reloadButton.click();
  else if (key === "d") devBox.click();
  else if (key === "l") highlightsBox.click();
  else if (key === "Escape") opts.selected = null;
  else handled = false;
  if (handled) e.preventDefault();
});

// ---------------------------------------------------------------------------
// Mouse: wheel zooms, drag pans, click selects, hover shows a tooltip
// ---------------------------------------------------------------------------

let drag: { x: number; y: number; moved: boolean } | null = null;

canvas.addEventListener("wheel", (e) => {
  e.preventDefault();
  cam.zoomAt(e.offsetX, e.offsetY, Math.exp(-e.deltaY * 0.0015));
  if (fixedZoom()) {
    zoomYards[viewMode()] = cam.acrossPixels / cam.scale;
    keepPanOffset();
    saveView();
  }
}, { passive: false });

canvas.addEventListener("pointerdown", (e) => {
  drag = { x: e.clientX, y: e.clientY, moved: false };
  canvas.setPointerCapture(e.pointerId);
});
canvas.addEventListener("pointermove", (e) => {
  if (drag) {
    const dx = e.clientX - drag.x;
    const dy = e.clientY - drag.y;
    if (drag.moved || Math.hypot(dx, dy) > 3) {
      drag.moved = true;
      canvas.classList.add("panning");
      cam.panPixels(dx, dy);
      if (fixedZoom()) keepPanOffset();
      drag.x = e.clientX;
      drag.y = e.clientY;
    }
    return;
  }
  hoverAt(e.offsetX, e.offsetY);
});
canvas.addEventListener("pointerup", (e) => {
  if (drag && !drag.moved && pb) opts.selected = playerAtPoint(cam, pb, e.offsetX, e.offsetY);
  drag = null;
  canvas.classList.remove("panning");
});
canvas.addEventListener("pointerleave", () => {
  opts.hovered = null;
  tooltip.hidden = true;
});
canvas.addEventListener("dblclick", () => (fixedZoom() ? (panOffset = [0, 0]) : fitPlayers()));

let hoverPoint: [number, number] | null = null;
function hoverAt(sx: number, sy: number): void {
  hoverPoint = [sx, sy];
}

// The hovered player moves even when the mouse doesn't, so this runs every frame.
function updateHover(): void {
  if (!pb || !hoverPoint || drag) return;
  const [sx, sy] = hoverPoint;
  opts.hovered = playerAtPoint(cam, pb, sx, sy);
  if (opts.hovered === null) {
    tooltip.hidden = true;
    canvas.style.cursor = "";
    return;
  }
  const info = pb.rec.header.players[opts.hovered]!;
  const p = pb.player(opts.hovered);
  tooltip.textContent = `${info.name} · ${info.position} · ${(Math.hypot(p.vx, p.vy) * 2.0455).toFixed(1)} mph`;
  tooltip.hidden = false;
  // Keep it inside the stage: flip to the left of the cursor near the right edge.
  const flip = sx + 14 + tooltip.offsetWidth > cam.width;
  tooltip.style.left = `${flip ? sx - 14 - tooltip.offsetWidth : sx + 14}px`;
  tooltip.style.top = `${sy + 14}px`;
  canvas.style.cursor = "pointer";
}

// ---------------------------------------------------------------------------
// The view: sideline (the whole field, left to right) or vertical (behind a
// team, the field's width across the screen, following the ball up and down)
// ---------------------------------------------------------------------------

const VIEW_KEY = "cfbsim.view";

function viewMode(): ViewMode {
  return viewSelect.value as ViewMode;
}

function saveView(): void {
  try {
    localStorage.setItem(
      VIEW_KEY,
      JSON.stringify({
        yards: zoomYards,
        dev: devBox.checked,
        highlights: highlightsBox.checked,
        detail: detailBox.checked,
      }),
    );
  } catch {
    // No storage (private window, blocked site data): the choice just isn't remembered.
  }
}

function restoreView(): void {
  try {
    const saved = JSON.parse(localStorage.getItem(VIEW_KEY) ?? "null");
    if (typeof saved?.dev === "boolean") devBox.checked = saved.dev;
    if (typeof saved?.highlights === "boolean") highlightsBox.checked = saved.highlights;
    if (typeof saved?.detail === "boolean") detailBox.checked = saved.detail;
    for (const mode of ["horizontal", "vertical"] as const) {
      const y = Number(saved?.yards?.[mode]);
      if (Number.isFinite(y) && y > 2) zoomYards[mode] = y;
    }
  } catch {
    // Nothing saved, or no storage.
  }
  applyDefaultView(topBar.defaults);
}

// The view, view position, game speed, and zoom the Defaults start with.
function applyDefaultView(d: Defaults): void {
  speedSelect.value = String(d.speed);
  if (pb) pb.speed = d.speed;
  zoomSelect.value = d.zoom;
  viewSelect.value = d.view;
  behindSelect.value = d.behind;
  behindSelect.hidden = viewMode() !== "vertical";
  applyView(true);
}

// Turns the camera for the current file and view. `refit` also reframes it
// (a new file, or the view changed); the vertical view always reframes, since
// it frames the same way every time.
function applyView(refit: boolean): void {
  const before = cam.turns;
  cam.turns = pb ? viewTurns(pb.rec, viewMode(), behindSelect.value as Behind) : viewMode() === "vertical" ? 1 : 0;
  if (fixedZoom() && pb) {
    if (refit) panOffset = [0, 0];
    frameFixed(); // (the draw loop keeps it framed)
  } else if (viewMode() === "vertical") fitView();
  else if (refit || cam.turns !== before) fitPlayers();
}

// "Fit": with fixed zoom, back to the default zoom for this view, on the
// ball; otherwise the whole field from the sideline, or the field's width
// centered on the ball in the vertical view.
function fitView(): void {
  if (fixedZoom() && pb) {
    zoomYards[viewMode()] = DEFAULT_YARDS[viewMode()];
    panOffset = [0, 0];
    saveView();
    frameFixed();
  } else if (viewMode() === "vertical") cam.fitWidthAt(verticalX());
  else cam.fitField();
}

// The point the camera centers on: the ball (or the players), and in the
// vertical view VERTICAL_LEAD yards up the screen from it.
function anchor(): [number, number] {
  const [x, y] = follow ?? [cam.cx, cam.cy];
  if (viewMode() !== "vertical") return [x, y];
  const [dx, dy] = cam.upBy(VERTICAL_LEAD);
  return [x + dx, y + dy];
}

// The unzoomed vertical view's center along the field.
function verticalX(): number {
  return follow == null ? 60 : follow[0] + cam.upBy(VERTICAL_LEAD)[0];
}

// Moves the followed point on by `dt` s of real time: eased while playing;
// paused, it holds, and a scrub or a step goes straight there.
function updateFollow(dt: number): void {
  const to = pb && followPoint(pb.rec, pb.frame);
  if (!to) return;
  if (pb!.playing || dt === 0) follow = easeToward(follow, to, dt * pb!.speed);
  else if (pb!.frame !== followFrame) follow = to;
  followFrame = pb!.frame;
}

function fixedZoom(): boolean {
  return zoomSelect.value === "fixed";
}

// Fixed zoom: the saved zoom for this view, following the ball (plus any drag).
function frameFixed(): void {
  if (!pb) return;
  const [x, y] = anchor();
  cam.frameOn(x + panOffset[0], y + panOffset[1], zoomYards[viewMode()]);
}

// After the user zooms or drags with fixed zoom: remember where the view sits
// relative to the ball, so it keeps following from there.
function keepPanOffset(): void {
  if (!pb) return;
  const [x, y] = anchor();
  panOffset = [cam.cx - x, cam.cy - y];
}

zoomSelect.addEventListener("change", () => {
  saveView();
  applyView(true);
  zoomSelect.blur();
});

for (const select of [viewSelect, behindSelect]) {
  select.addEventListener("change", () => {
    behindSelect.hidden = viewMode() !== "vertical";
    saveView();
    applyView(true);
    select.blur(); // so the keyboard shortcuts keep working
  });
}
restoreView();

// Frames the area every player covers during the whole recording.
function fitPlayers(): void {
  if (!pb) return cam.fitField();
  const { state } = pb.rec;
  let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
  for (let k = 0; k < state.length; k += STRIDE) {
    minX = Math.min(minX, state[k + X]!);
    maxX = Math.max(maxX, state[k + X]!);
    minY = Math.min(minY, state[k + Y]!);
    maxY = Math.max(maxY, state[k + Y]!);
  }
  const pad = 6;
  cam.fit(minX - pad, minY - pad, maxX + pad, maxY + pad);
}

// ---------------------------------------------------------------------------
// The draw loop
// ---------------------------------------------------------------------------

// Keeps the canvas's pixel size matched to its on-screen size, times the
// device pixel ratio, so lines stay sharp on high-DPI screens.
function resize(): void {
  const rect = canvas.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  canvas.width = Math.round(rect.width * dpr);
  canvas.height = Math.round(rect.height * dpr);
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  const first = cam.width === 1;
  cam.resize(rect.width, rect.height);
  // The file can finish loading before the canvas has a size; fit again now.
  if (first) applyView(true);
  else if (!fixedZoom() && viewMode() === "vertical") fitView(); // keep the field's width filling the screen
}
new ResizeObserver(resize).observe(canvas);

let lastTime = performance.now();
function loop(now: number): void {
  const dt = Math.min((now - lastTime) / 1000, 0.1); // don't jump after a background tab
  lastTime = now;
  if (pb) {
    pb.advance(dt);
    updateFollow(dt);
    // Fixed zoom follows the ball; so does the vertical camera, yard for yard.
    if (fixedZoom()) frameFixed();
    else if (viewMode() === "vertical") cam.cx = pb.rec.header.players.length ? verticalX() : cam.cx;
    updateHover();
    drawScene(ctx, cam, pb, opts);
    updatePanel();
  } else {
    drawEmptyField();
  }
  live.tick(dt);
  requestAnimationFrame(loop);
}

function drawEmptyField(): void {
  drawScene(ctx, cam, emptyPlayback, opts);
}
const emptyPlayback = new Playback(
  parseTracking('{"type":"header","format_version":1,"tick_rate":20,"players":[]}\n{"type":"tick","tick":0,"p":[],"ball":null}'),
);

requestAnimationFrame(loop);
{
  const m = /#file=(.+)$/.exec(location.hash);
  if (m) void loadRepoFile(decodeURIComponent(m[1]!));
}
