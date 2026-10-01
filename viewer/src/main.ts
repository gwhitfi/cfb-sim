// Wires the page together: loading files, the playback loop, mouse and
// keyboard controls, and the side panel.

import { Camera } from "./camera";
import { Playback, SPEEDS } from "./playback";
import { drawScene, layerColor, playerAtPoint, type ViewOptions } from "./render";
import { describeEvent } from "./events";
import { parseTracking, TrackingError, X, Y, STRIDE, type Recording } from "./tracking";

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;

const canvas = $<HTMLCanvasElement>("canvas");
const ctx = canvas.getContext("2d")!;
const stage = $<HTMLDivElement>("stage");
const scrub = $<HTMLInputElement>("scrub");
const playButton = $<HTMLButtonElement>("play");
const speedSelect = $<HTMLSelectElement>("speed");
const repoFiles = $<HTMLSelectElement>("repo-files");
const reloadButton = $<HTMLButtonElement>("reload");
const tooltip = $<HTMLDivElement>("tooltip");

const PLAY_ICON = `<svg viewBox="0 0 16 16" width="14" height="14"><path d="M4 2v12l10-6z" fill="currentColor"/></svg>`;
const PAUSE_ICON = `<svg viewBox="0 0 16 16" width="14" height="14"><path d="M3 2h4v12H3zM9 2h4v12H9z" fill="currentColor"/></svg>`;
playButton.innerHTML = PLAY_ICON;

const cam = new Camera();
let pb: Playback | null = null;
let source: { name: string; repoPath?: string } | null = null;

const opts: ViewOptions = {
  velocity: false,
  trails: false,
  labelMode: "label",
  hiddenLayers: new Set(),
  selected: null,
  hovered: null,
};

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

function load(text: string, name: string, repoPath?: string): void {
  let rec: Recording;
  try {
    rec = parseTracking(text);
  } catch (e) {
    showError(e instanceof TrackingError ? `${name}: ${e.message}` : String(e));
    return;
  }
  // Reloading the same file keeps the view and the moment you were looking at.
  const sameFile = source?.name === name && pb !== null;
  const keepFrame = sameFile ? pb!.frame : 0;
  source = { name, repoPath };
  pb = new Playback(rec);
  pb.seek(keepFrame);
  if (!sameFile) {
    opts.selected = null;
    opts.hiddenLayers.clear();
    fitPlayers();
  }
  if (repoPath) history.replaceState(null, "", `#file=${encodeURIComponent(repoPath)}`);

  $("empty").hidden = true;
  $("error").hidden = true;
  reloadButton.disabled = !repoPath;
  const h = rec.header;
  $("title").textContent = `${name} — ${h.kind} "${h.name}"`;
  scrub.max = String(rec.frameCount - 1);
  buildInfo(rec);
  buildLayers(rec);
  buildEvents(rec);
  updatePanel();
}

function showError(message: string): void {
  const el = $("error");
  el.textContent = message;
  el.hidden = false;
}

async function loadFile(file: File): Promise<void> {
  load(await file.text(), file.name);
}

// The dev server (vite.config.ts) lists and serves tracking files from the
// repo, so you can rerun the CLI and hit Reload. A production build has no
// such endpoint, and the picker stays hidden.
async function refreshRepoFiles(): Promise<void> {
  try {
    const res = await fetch("/api/tracking");
    if (!res.ok) return;
    const files: string[] = await res.json();
    repoFiles.replaceChildren(new Option("Repo files…", ""), ...files.map((f) => new Option(f, f)));
    repoFiles.value = source?.repoPath ?? "";
    repoFiles.hidden = false;
  } catch {
    // Not running under the dev server.
  }
}

async function loadRepoFile(path: string): Promise<void> {
  const res = await fetch(`/api/tracking/file?path=${encodeURIComponent(path)}`);
  if (!res.ok) {
    showError(`${path}: ${res.status} ${await res.text()}`);
    return;
  }
  load(await res.text(), path.split("/").pop()!, path);
  repoFiles.value = path;
}

$<HTMLInputElement>("file-input").addEventListener("change", (e) => {
  const file = (e.target as HTMLInputElement).files?.[0];
  if (file) void loadFile(file);
});
repoFiles.addEventListener("focus", () => void refreshRepoFiles());
repoFiles.addEventListener("change", () => {
  if (repoFiles.value) void loadRepoFile(repoFiles.value);
});
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
  for (const [k, v] of Object.entries(h.details)) rows.push([k, JSON.stringify(v)]);
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

function buildEvents(rec: Recording): void {
  const list = $("events");
  list.replaceChildren(
    ...rec.events.map((ev) => {
      const li = document.createElement("li");
      const t = document.createElement("span");
      t.className = "t";
      t.textContent = `${((ev.tick - rec.ticks[0]!) / rec.header.tickRate).toFixed(2)}s`;
      const text = document.createElement("span");
      text.textContent = describeEvent(ev);
      li.append(t, text);
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
});

function changeSpeed(dir: 1 | -1): void {
  const i = SPEEDS.indexOf(Number(speedSelect.value) as (typeof SPEEDS)[number]);
  const next = SPEEDS[Math.min(Math.max(i + dir, 0), SPEEDS.length - 1)]!;
  speedSelect.value = String(next);
  if (pb) pb.speed = next;
}

playButton.addEventListener("click", () => pb?.togglePlay());
$("back").addEventListener("click", () => pb?.step(-1));
$("fwd").addEventListener("click", () => pb?.step(1));
scrub.addEventListener("input", () => {
  if (!pb) return;
  pb.playing = false;
  pb.seek(Number(scrub.value));
});
$("fit").addEventListener("click", (e) => (e.shiftKey ? fitPlayers() : cam.fitField()));

const velocityBox = $<HTMLInputElement>("opt-velocity");
const trailsBox = $<HTMLInputElement>("opt-trails");
const labelsSelect = $<HTMLSelectElement>("opt-labels");
velocityBox.addEventListener("change", () => (opts.velocity = velocityBox.checked));
trailsBox.addEventListener("change", () => (opts.trails = trailsBox.checked));
labelsSelect.addEventListener("change", () => (opts.labelMode = labelsSelect.value as ViewOptions["labelMode"]));

window.addEventListener("keydown", (e) => {
  if (e.target instanceof HTMLInputElement && e.target.type !== "range" && e.target.type !== "checkbox") return;
  if (e.target instanceof HTMLSelectElement) return;
  const key = e.key;
  let handled = true;
  if (key === " ") pb?.togglePlay();
  else if (key === "ArrowRight") e.shiftKey ? pb?.seek(pb.frame + pb.rec.header.tickRate) : pb?.step(1);
  else if (key === "ArrowLeft") e.shiftKey ? pb?.seek(pb.frame - pb.rec.header.tickRate) : pb?.step(-1);
  else if (key === "Home") pb?.seek(0);
  else if (key === "End") pb?.seek(pb.lastFrame);
  else if (key === "]") changeSpeed(1);
  else if (key === "[") changeSpeed(-1);
  else if (key === "f") cam.fitField();
  else if (key === "F") fitPlayers();
  else if (key === "v") velocityBox.click();
  else if (key === "t") trailsBox.click();
  else if (key === "r") reloadButton.click();
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
canvas.addEventListener("dblclick", () => fitPlayers());

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
  if (first) {
    if (pb) fitPlayers();
    else cam.fitField();
  }
}
new ResizeObserver(resize).observe(canvas);

let lastTime = performance.now();
function loop(now: number): void {
  const dt = Math.min((now - lastTime) / 1000, 0.1); // don't jump after a background tab
  lastTime = now;
  if (pb) {
    pb.advance(dt);
    updateHover();
    drawScene(ctx, cam, pb, opts);
    updatePanel();
  } else {
    drawEmptyField();
  }
  requestAnimationFrame(loop);
}

function drawEmptyField(): void {
  drawScene(ctx, cam, emptyPlayback, opts);
}
const emptyPlayback = new Playback(
  parseTracking('{"type":"header","format_version":1,"tick_rate":20,"players":[]}\n{"type":"tick","tick":0,"p":[],"ball":null}'),
);

requestAnimationFrame(loop);
void refreshRepoFiles().then(() => {
  const m = /#file=(.+)$/.exec(location.hash);
  if (m) void loadRepoFile(decodeURIComponent(m[1]!));
});
