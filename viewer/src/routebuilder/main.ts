// The Route Builder page (/routebuilder): draw a route template on a field and
// copy its JSON into data/library/routes.json by hand. It saves nothing but a
// draft in this browser; its one request reads the existing routes (read
// only) through the dev server's /api/library, to start from one.

import { el, load, save } from "../builder/dom";
import { hashX, makeFrame, numbersX, sidelineX, type Hash } from "../playbuilder/anchors";
import { button, checkbox, field, numberInput, segmented, select, textInput } from "../playbuilder/ui";
import "../playbuilder/playbuilder.css";
import "./routebuilder.css";
import { END_HELP, ENDS, fromLibrary, newRoute, onField, problems, routeText, toRoute, type End, type RouteDraft } from "./route";

const $ = <T extends Element = HTMLElement>(id: string) => document.getElementById(id) as unknown as T;
type LibRoute = Parameters<typeof fromLibrary>[1];

// Where the receiver lines up for the picture. The route is the same from any
// of them: +x always turns toward his own sideline.
const SPOTS = {
  right_numbers: "right, on the numbers (Z)",
  right_slot: "right slot",
  right_te: "right, tight end",
  left_numbers: "left, on the numbers (X)",
  left_slot: "left slot",
  left_te: "left, tight end",
  back_right: "back, releasing right",
  back_left: "back, releasing left",
} as const;
type Spot = keyof typeof SPOTS;

interface State { v: 1; route: RouteDraft; spot: Spot; hash: Hash; mirror: boolean }
const STORAGE = "cfbsim.routebuilder";
const saved = load(STORAGE) as State | null;
const state: State = saved?.v === 1 && saved.route ? saved : { v: 1, route: newRoute(), spot: "right_numbers", hash: "middle", mirror: true };
let library: Record<string, LibRoute> = {};
let selected = -1; // the point being dragged or last clicked

function receiver(spot: Spot): { x: number; y: number; side: "left" | "right" } {
  const f = makeFrame(state.hash);
  const side = spot.startsWith("left") || spot === "back_left" ? "left" : "right";
  const s = side === "left" ? -1 : 1;
  if (spot.endsWith("numbers")) return { x: numbersX(f, side), y: -1, side };
  if (spot.endsWith("slot")) return { x: numbersX(f, side) - s * 6, y: -2, side };
  if (spot.endsWith("te")) return { x: s * 4.5, y: -1, side };
  return { x: s * 0.5, y: -5, side };
}
const mirrorOf = (spot: Spot): Spot => (spot.startsWith("left") ? spot.replace("left", "right") : spot.replace("right", "left")) as Spot;

// ---------------------------------------------------------------------------
// The left panel
// ---------------------------------------------------------------------------

function renderLeft(): void {
  const r = state.route;
  const names = Object.keys(library);
  const pointRows = r.points.map((p, i) => el("div", { className: `pt${i === selected ? " on" : ""}` },
    el("b", { textContent: `${i + 1}` }),
    field("x", numberInput(p.x, (v) => { p.x = v ?? 0; redraw(); }, 0.5, "Yards outside (+) or inside (-) of where he lines up")),
    field("y", numberInput(p.y, (v) => { p.y = v ?? 0; redraw(); }, 0.5, "Yards downfield of where he lines up")),
    field("break", checkbox(p.brk, (c) => { p.brk = c; redraw(); }, "A planted cut. A play's depth override moves the route from the first break.")),
    field("decel", checkbox(p.decel, (c) => { p.decel = c; redraw(); }, "Throttles down into this point (a curl, a hitch)")),
    ...(i > 0 ? [button("↑", () => { r.points.splice(i - 1, 0, r.points.splice(i, 1)[0]!); renderLeft(); redraw(); }, "Move earlier", "mini")] : []),
    button("×", () => { r.points.splice(i, 1); selected = -1; renderLeft(); redraw(); }, "Remove", "mini"),
  ));
  $("left").replaceChildren(
    el("section", { className: "panel" }, el("h3", { textContent: "Route" }),
      el("div", { className: "row" }, field("start from", select([["", "(pick a route)"], ...names], "", (v) => {
        if (!v) return;
        state.route = fromLibrary(v, library[v]!);
        selected = -1;
        renderLeft();
        redraw();
      }, "Load a route from data/library/routes.json to change or copy")),
      button("New", () => { state.route = newRoute(); selected = -1; renderLeft(); redraw(); })),
      el("div", { className: "row" }, field("name", textInput(r.name, (v) => { r.name = v.trim(); redraw(); }))),
      el("div", { className: "row" }, field("end", segmented(ENDS, r.end, (v) => { r.end = v as End; renderLeft(); redraw(); }))),
      el("p", { className: "muted note", textContent: `${r.end}: ${END_HELP[r.end]}.` }),
    ),
    el("section", { className: "panel" }, el("h3", { textContent: "Points (from where he lines up; +x is outside)" }),
      ...pointRows,
      el("div", { className: "row" }, button("+ point", () => {
        const last = r.points[r.points.length - 1] ?? { x: 0, y: 0 };
        r.points.push({ x: last.x, y: last.y + 3, brk: false, decel: false });
        renderLeft();
        redraw();
      }, "", "mini"), el("span", { className: "muted", textContent: "or click the field" })),
    ),
    el("section", { className: "panel" }, el("h3", { textContent: "Picture" }),
      el("div", { className: "row" }, field("receiver", select(Object.entries(SPOTS), state.spot, (v) => { state.spot = v as Spot; redraw(); }))),
      el("div", { className: "row" },
        field("ball on", select([["left", "left hash"], ["middle", "middle"], ["right", "right hash"]], state.hash, (v) => { state.hash = v as Hash; redraw(); })),
        field("mirror", checkbox(state.mirror, (c) => { state.mirror = c; redraw(); }, "Also show it run from the other side"))),
    ),
    el("ul", { id: "problems" }),
    el("div", { className: "out" },
      el("div", { className: "out-head" }, el("strong", { textContent: "routes.json entry" }), el("span", { className: "muted", textContent: 'goes inside "routes"' }),
        button("Copy", () => {
          const b = document.querySelector<HTMLButtonElement>(".out-head button")!;
          void navigator.clipboard.writeText(routeText(state.route)).then(() => {
            b.textContent = "Copied";
            setTimeout(() => (b.textContent = "Copy"), 1200);
          });
        })),
      el("textarea", { id: "json", readOnly: true, spellcheck: false }),
    ),
  );
  redraw();
}

// ---------------------------------------------------------------------------
// The field
// ---------------------------------------------------------------------------

const NS = "http://www.w3.org/2000/svg";
const S = <K extends keyof SVGElementTagNameMap>(tag: K, attrs: Record<string, string | number>, text?: string): SVGElementTagNameMap[K] => {
  const e = document.createElementNS(NS, tag);
  for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, String(v));
  if (text !== undefined) e.textContent = text;
  return e;
};
const Y_TOP = 42;
const Y_BOTTOM = -8;
const svg = $<SVGSVGElement>("field");

function arrowHead(a: { x: number; y: number }, b: { x: number; y: number }, color: string, w: number): SVGPolylineElement {
  const ang = Math.atan2(-(b.y - a.y), b.x - a.x);
  const arm = (d: number) => `${b.x - Math.cos(ang + d)},${-b.y - Math.sin(ang + d)}`;
  return S("polyline", { points: `${arm(0.45)} ${b.x},${-b.y} ${arm(-0.45)}`, fill: "none", stroke: color, "stroke-width": w });
}

function drawRoute(spot: Spot, main: boolean): void {
  const r = state.route;
  const rec = receiver(spot);
  const path = onField(r, rec.x, rec.y, rec.side);
  const color = main ? "#ffd400" : "#ffd40066";
  const w = main ? 0.3 : 0.2;
  const g = S("g", {});
  g.append(S("polyline", { points: path.map((p) => `${p.x},${-p.y}`).join(" "), fill: "none", stroke: color, "stroke-width": w, "stroke-linejoin": "round" }));
  const [a, b] = [path[path.length - 2], path[path.length - 1]];
  if (a && b && r.points.length) {
    if (r.end === "continue") g.append(arrowHead(a, b, color, w));
    else if (r.end === "stop") {
      const ang = Math.atan2(-(b.y - a.y), b.x - a.x);
      const [dx, dy] = [-Math.sin(ang) * 0.9, Math.cos(ang) * 0.9];
      g.append(S("line", { x1: b.x - dx, y1: -b.y - dy, x2: b.x + dx, y2: -b.y + dy, stroke: color, "stroke-width": w + 0.1 }));
    } else g.append(S("circle", { cx: b.x, cy: -b.y, r: 1.2, fill: "none", stroke: color, "stroke-width": w, "stroke-dasharray": "0.4 0.3" }));
  }
  g.append(S("circle", { cx: rec.x, cy: -rec.y, r: 0.95, fill: main ? "#5aa9ff" : "#5aa9ff66", stroke: "#000", "stroke-width": 0.1 }));
  if (main) {
    // Which way is "outside" for this receiver.
    const s = rec.side === "left" ? -1 : 1;
    g.append(S("line", { x1: rec.x, y1: -rec.y + 2, x2: rec.x + s * 3, y2: -rec.y + 2, stroke: "#ffffff88", "stroke-width": 0.12 }));
    g.append(arrowHead({ x: rec.x, y: rec.y - 2 }, { x: rec.x + s * 3, y: rec.y - 2 }, "#ffffff88", 0.12));
    g.append(S("text", { x: rec.x + s * 1.5, y: -rec.y + 3.2, "font-size": 0.8, fill: "#ffffffaa", "text-anchor": "middle" }, "+x outside"));
    r.points.forEach((p, i) => {
      const at = path[i + 1]!;
      const h = S("g", { class: "piece", "data-pt": i });
      if (p.decel) h.append(S("rect", { x: at.x - 0.85, y: -at.y - 0.85, width: 1.7, height: 1.7, fill: "none", stroke: "#ff9f43", "stroke-width": 0.18 }));
      h.append(S("circle", { cx: at.x, cy: -at.y, r: 0.6, fill: p.brk ? "#ffd400" : "#1b3524", stroke: i === selected ? "#fff" : "#ffd400", "stroke-width": i === selected ? 0.25 : 0.15 }));
      h.append(S("text", { x: at.x, y: -at.y + 0.28, "font-size": 0.75, "text-anchor": "middle", fill: p.brk ? "#000" : "#ffd400", "font-weight": "bold" }, String(i + 1)));
      h.append(S("title", {}, `point ${i + 1}: x ${p.x}, y ${p.y}${p.brk ? ", break" : ""}${p.decel ? ", decel" : ""}`));
      g.append(h);
    });
  }
  svg.append(g);
}

function redraw(): void {
  const f = makeFrame(state.hash);
  const [left, right] = [sidelineX(f, "left"), sidelineX(f, "right")];
  svg.setAttribute("viewBox", `${left - 1} ${-Y_TOP} ${right - left + 2} ${Y_TOP - Y_BOTTOM}`);
  svg.replaceChildren(S("rect", { x: left, y: -Y_TOP, width: right - left, height: Y_TOP - Y_BOTTOM, fill: "#20402a" }));
  const line = (x1: number, y1: number, x2: number, y2: number, stroke: string, w: number) => svg.append(S("line", { x1, y1: -y1, x2, y2: -y2, stroke, "stroke-width": w }));
  for (let y = Y_BOTTOM; y <= Y_TOP; y++) {
    if (y % 5 === 0 && y !== 0) {
      line(left, y, right, y, "#ffffff40", 0.1);
      svg.append(S("text", { x: left + 0.5, y: -y - 0.25, "font-size": 0.8, fill: "#ffffff66" }, `${y}`));
    } else line(left, y, right, y, "#ffffff10", 0.05);
    for (const s of ["left", "right"] as const) line(hashX(f, s) - 0.4, y, hashX(f, s) + 0.4, y, "#ffffff40", 0.08);
  }
  for (const s of ["left", "right"] as const) line(sidelineX(f, s), Y_BOTTOM, sidelineX(f, s), Y_TOP, "#ffffffaa", 0.25);
  line(left, 0, right, 0, "#ffd400", 0.15);
  svg.append(S("text", { x: right - 0.5, y: -0.4, "font-size": 0.8, fill: "#ffd400", "text-anchor": "end" }, "LOS"));
  svg.append(S("circle", { cx: 0, cy: 0, r: 0.35, fill: "#c8834a" }));
  if (state.mirror) drawRoute(mirrorOf(state.spot), false);
  drawRoute(state.spot, true);

  const probs = problems(state.route, Object.keys(library));
  const ul = document.getElementById("problems");
  if (ul) {
    ul.className = probs.length ? "bad" : "good";
    ul.replaceChildren(...(probs.length ? probs.map((m) => el("li", { textContent: m })) : [el("li", { textContent: "Ready: paste the entry into routes.json and run cfbsim validate." })]));
  }
  const ta = document.getElementById("json") as HTMLTextAreaElement | null;
  if (ta) ta.value = routeText(state.route);
  save(STORAGE, state);
}

// ---- clicking and dragging: in route-frame yards, snapped

function toField(e: PointerEvent): { x: number; y: number } {
  const m = svg.getScreenCTM();
  const p = m ? new DOMPoint(e.clientX, e.clientY).matrixTransform(m.inverse()) : { x: 0, y: 0 };
  return { x: p.x, y: -p.y };
}
const toRouteFrame = (e: PointerEvent) => {
  const rec = receiver(state.spot);
  const f = toField(e);
  return toRoute(f.x, f.y, rec.x, rec.y, rec.side, e.shiftKey ? 0.1 : 0.5);
};

let dragging = false;
svg.addEventListener("pointerdown", (e) => {
  const t = (e.target as Element).closest("[data-pt]");
  if (t) selected = Number(t.getAttribute("data-pt"));
  else {
    state.route.points.push({ ...toRouteFrame(e), brk: false, decel: false });
    selected = state.route.points.length - 1;
  }
  dragging = true;
  svg.setPointerCapture(e.pointerId);
  renderLeft();
  e.preventDefault();
});
svg.addEventListener("pointermove", (e) => {
  const at = toRouteFrame(e);
  $("readout").textContent = `pointer: x ${at.x} (${at.x >= 0 ? "outside" : "inside"}), y ${at.y} from where he lines up`;
  const p = state.route.points[selected];
  if (!dragging || !p) return;
  p.x = at.x;
  p.y = at.y;
  redraw();
});
const stop = () => {
  if (dragging) renderLeft(); // the point's numbers
  dragging = false;
};
svg.addEventListener("pointerup", stop);
svg.addEventListener("pointercancel", stop);

async function start(): Promise<void> {
  try {
    library = ((await (await fetch("/api/library")).json()) as { routes: Record<string, LibRoute> }).routes;
  } catch {
    library = {}; // the page still works; there's just nothing to start from
  }
  renderLeft();
}
void start();
