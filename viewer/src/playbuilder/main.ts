// The Play Builder page (/playbuilder): build an offensive or defensive
// formation and play on a mini field, then copy the JSON into data/ by hand.
// Click a player to edit him, drag him to move. Existing formations and plays
// come in by pasting their JSON (Import). The page saves nothing anywhere but
// a draft in this browser's localStorage; its one request is the read-only
// route, zone, blocking, and QB library from data/library/ (/api/library).
//
// The logic lives in DOM-free modules (offense.ts, defense.ts, anchors.ts,
// draw.ts's layout, paste.ts); this file is the page: state, panels, and events.

import { el, load, save } from "../builder/dom";
import { snap, withX, type Anchor, type Ctx, type Gap, type Hash, type Side, type SideWord, GAPS, SIDE_WORDS } from "./anchors";
import * as D from "./defense";
import { EMPTY_LIBRARY, draw, describeOff, describePass, describeRun, layout, routePoints, type Layout, type Library, type Scene } from "./draw";
import * as O from "./offense";
import { readPasted, type Obj, type Pasted } from "./paste";
import { anchorEditor, button, checkbox, chips, field, numberInput, orderedList, segmented, select, textInput } from "./ui";
import "./playbuilder.css";

const $ = <T extends Element = HTMLElement>(id: string) => document.getElementById(id) as unknown as T;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

type Mode = "offense" | "defense";
interface OffState { formation: O.OffFormation; play: O.OffensePlay; baseline: string }
interface DefState { formation: D.DefFormation; play: D.DefensePlay; calls: string[]; override: Side | ""; view: "pass" | "run"; baseline: string }
interface State {
  v: 2;
  mode: Mode;
  hash: Hash;
  off: OffState;
  def: DefState;
  selected: Record<Mode, string>;
}

const STORAGE = "cfbsim.playbuilder";
const offBaseline = (f: O.OffFormation) => JSON.stringify(O.formationJson(f));
const defBaseline = (f: D.DefFormation) => JSON.stringify(D.formationJson(f));

function freshOff(): OffState {
  const formation = O.defaultOffenseFormation();
  return { formation, play: O.defaultOffensePlay(), baseline: offBaseline(formation) };
}
function freshDef(): DefState {
  const formation = D.defaultDefenseFormation();
  return { formation, play: D.defaultDefensePlay(), calls: ["te", "passing_strength", "field"], override: "", view: "pass", baseline: defBaseline(formation) };
}
function freshState(): State {
  return { v: 2, mode: "offense", hash: "middle", off: freshOff(), def: freshDef(), selected: { offense: "", defense: "" } };
}

function loadState(): State {
  const s = load(STORAGE) as State | null;
  return s && s.v === 2 && s.off?.formation && s.def?.formation ? s : freshState();
}

let state = loadState();
let lib: Library = EMPTY_LIBRARY;
let lay: Layout = layout(scene());
let notice: { text: string; bad: boolean } | null = null;

const keys = (o: object) => Object.keys(o);
const libNames = (): O.LibNames => ({
  routes: keys(lib.routes),
  drops: keys(lib.qb.drops),
  fakes: keys(lib.qb.fakes),
  protections: keys(lib.blocking.protections),
  runSchemes: keys(lib.blocking.run_schemes),
});

function scene(): Scene {
  const d = state.def;
  return {
    mode: state.mode,
    hash: state.hash,
    lib,
    off: { formation: state.off.formation, play: state.off.play },
    def: { formation: d.formation, play: d.play, calls: d.calls, override: d.override, view: d.view },
    selected: state.selected[state.mode],
  };
}

// The side of the ball an offensive slot lines up on, from the last layout.
const offSide = (name: string): Side => ((lay.off[name]?.x ?? 0) < 0 ? "left" : "right");
// Strong or weak for a defender, from the last layout.
function defSide(name: string): "strong" | "weak" {
  const r = lay.read;
  const x = lay.def[name]?.x;
  if (!r || x === undefined) return "strong";
  return (x < r.centerX ? "left" : "right") === r.strong ? "strong" : "weak";
}

function normalize(): void {
  O.normalizeOffense(state.off.formation, state.off.play, offSide, keys(lib.routes));
  D.normalizeDefense(state.def.formation, state.def.play, defSide, keys(lib.zones));
}

const offSlot = (name: string) => state.off.formation.slots.find((s) => s.name === name);
const defSlot = (name: string) => state.def.formation.slots.find((s) => s.name === name);

// ---------------------------------------------------------------------------
// Rendering: renderAll rebuilds every panel (after a change to what the
// panels show); refresh redraws the field, the roster, the checks, and the
// JSON (after a value typed into a panel, so the box being typed in stays).
// ---------------------------------------------------------------------------

function renderAll(): void {
  normalize();
  lay = layout(scene());
  $("btn-offense").classList.toggle("on", state.mode === "offense");
  $("btn-defense").classList.toggle("on", state.mode === "defense");
  $<HTMLSelectElement>("hash").value = state.hash;
  $("left").replaceChildren(...(state.mode === "offense" ? offenseLeft() : defenseLeft()), el("div", { id: "roster" }));
  renderInspector();
  refresh();
}

function refresh(): void {
  normalize();
  lay = layout(scene());
  draw($<SVGSVGElement>("field"), scene(), lay);
  renderRoster();
  renderNotice();
  renderChecks();
  renderOutput();
  save(STORAGE, state);
}

function renderNotice(): void {
  const n = $("notice");
  n.hidden = !notice;
  if (!notice) return;
  n.className = notice.bad ? "bad" : "";
  n.replaceChildren(el("span", { textContent: notice.text }), button("×", () => { notice = null; renderNotice(); }, "Dismiss", "mini"));
}

function say(text: string, bad = false): void {
  notice = { text, bad };
  renderNotice();
}

const section = (title: string, ...children: (Node | string)[]) => el("section", { className: "panel" }, el("h3", { textContent: title }), ...children);
const row = (...children: (Node | string)[]) => el("div", { className: "row" }, ...children);
const note = (text: string) => el("p", { className: "muted note", textContent: text });

// ---- the roster: every slot, what he does; click to edit, + to add

function renderRoster(): void {
  const box = document.getElementById("roster");
  if (!box) return;
  const sel = state.selected[state.mode];
  const items: HTMLElement[] = [];
  if (state.mode === "offense") {
    const p = state.off.play;
    const map = O.currentAssignments(p);
    for (const s of state.off.formation.slots) {
      const a = map[s.name];
      const what = O.isCarrier(s, p) ? "ball carrier" : a ? describeOff(a, p) : "—";
      items.push(rosterItem(s.name, s.pos, what, sel === s.name));
    }
  } else {
    const p = state.def.play;
    for (const s of state.def.formation.slots) {
      const a = p.assignments[s.name];
      const what = a ? `${describeRun(a.run)} · ${describePass(a.pass)}` : "—";
      items.push(rosterItem(s.name, s.pos, what, sel === s.name));
    }
  }
  const positions = state.mode === "offense" ? O.OFFENSE_POSITIONS : D.DEFENSE_POSITIONS;
  const count = items.length;
  box.replaceChildren(section(`Players (${count})`, el("div", { className: "roster" }, ...items),
    row(el("span", { className: "muted", textContent: "add a" }), select([["", "position…"], ...positions], "", (pos) => addSlot(pos)),
      ...(count !== 11 ? [el("span", { className: "bad", textContent: `${count}/11` })] : [])),
  ));
}

function rosterItem(name: string, pos: string, what: string, on: boolean): HTMLElement {
  return el("button", { type: "button", className: `ros${on ? " on" : ""}`, onclick: () => selectSlot(on ? "" : name) },
    el("b", { textContent: name }), el("span", { className: "muted", textContent: pos }), el("span", { className: "what", textContent: what }));
}

function selectSlot(name: string): void {
  state.selected[state.mode] = name;
  renderInspector();
  refresh();
}

// ---- checks: what the loader would reject

function renderChecks(): void {
  const probs: string[] = [];
  if (lay.error) probs.push(lay.error);
  if (state.mode === "offense") {
    const { formation: f, play: p } = state.off;
    probs.push(...O.formationProblems(f).map((m) => `Formation: ${m}`), ...O.playProblems(f, p, libNames()).map((m) => `Play: ${m}`));
  } else {
    const d = state.def;
    probs.push(...D.formationProblems(d.formation).map((m) => `Formation: ${m}`), ...D.playProblems(d.formation, d.play, d.calls, keys(lib.zones)).map((m) => `Play: ${m}`));
  }
  const ul = $("problems");
  ul.className = probs.length ? "bad" : "good";
  ul.replaceChildren(...(probs.length ? probs.map((m) => el("li", { textContent: m })) : [el("li", { textContent: "Ready: the loader should take this. Paste it into data/ and run cfbsim validate." })]));
  $("legend").textContent = state.mode === "offense"
    ? "Yellow: routes (orange/cyan dashed: vs man / vs zone options). Red: the primary target's route (read #1). Numbered badges: the read order. Red dashed: the QB's drop or fake. Green: the run path (drag its points)."
    : `Showing each defender's ${state.def.view} job. Offensive receivers are labeled as the defense numbers them (#1s = #1 strong, b1 = back 1).`;
}

// ---- the JSON to copy

function renderOutput(): void {
  const out: HTMLElement[] = [];
  const block = (title: string, sub: string, value: unknown, extraClass = "") => {
    const text = typeof value === "string" ? value : JSON.stringify(value, null, 2);
    const ta = el("textarea", { readOnly: true, spellcheck: false, value: text });
    const copy = button("Copy", () => {
      void navigator.clipboard.writeText(text).then(() => {
        copy.textContent = "Copied";
        setTimeout(() => (copy.textContent = "Copy"), 1200);
      });
    });
    out.push(el("div", { className: `out ${extraClass}` }, el("div", { className: "out-head" }, el("strong", { textContent: title }), el("span", { className: "muted", textContent: sub }), copy), ta));
  };
  if (state.mode === "offense") {
    const { formation: f, play: p, baseline } = state.off;
    const changed = offBaseline(f) !== baseline;
    block("Play", `goes in a playbook's "plays"; its formation "${f.id}" must be in the playbook's "formations"`, O.playJson(f, p));
    block("Formation", changed ? `changed: data/formations/offense/${f.id}.json` : "unchanged from the imported formation: not needed", O.formationJson(f), changed ? "" : "dim");
  } else {
    const d = state.def;
    const changed = defBaseline(d.formation) !== d.baseline;
    block("Play", `goes in a defensive playbook's "plays"; its formation "${d.formation.id}" must be in the playbook's "formations"`, D.playJson(d.formation, d.play));
    block("Formation", changed ? `changed: data/formations/defense/${d.formation.id}.json` : "unchanged from the imported formation: not needed", D.formationJson(d.formation), changed ? "" : "dim");
    block("Strength call", "the playbook's top-level line (shared by all its plays)", `"strength_call": ${JSON.stringify(d.calls)}`, "short");
  }
  $("output").replaceChildren(...out);
}

// ---------------------------------------------------------------------------
// The left panel: the play and the formation
// ---------------------------------------------------------------------------

function offenseLeft(): HTMLElement[] {
  const { formation: f, play: p } = state.off;
  const routers = O.routers(f, p);
  const typeRow = row(
    field("type", segmented(["pass", "run"], p.type, (v) => { p.type = v as O.PlayType; renderAll(); })),
    field("flip", checkbox(p.flip, (c) => { p.flip = c; refresh(); }, "The play can also be called mirrored")),
  );
  const play = section("Play",
    row(field("id", textInput(p.id, (v) => { p.id = v; refresh(); })), field("name", textInput(p.name, (v) => { p.name = v; refresh(); }))),
    typeRow,
  );
  if (p.type === "pass") {
    const s = p.pass;
    play.append(
      row(field("drop", select(keys(lib.qb.drops), s.drop, (v) => { s.drop = v; refresh(); }, "data/library/qb_actions.json")),
        field("read", select(O.READ_TYPES, s.readType, (v) => { s.readType = v; refresh(); }, "Only 'progression' is built in the sim"))),
      row(field("protection", select(keys(lib.blocking.protections), s.protection, (v) => { s.protection = v; refresh(); })),
        field("slide", select([["", "(none)"], "left", "right"], s.direction, (v) => { s.direction = v as Side | ""; refresh(); }))),
      el("div", { className: "sub" }, el("span", { className: "lbl", textContent: "progression (the QB's read order)" }),
        orderedList(s.progression, routers.filter((n) => !s.progression.includes(n)), (v) => { s.progression = v; renderAll(); })),
      row(field("hot", select([["", "(none)"], ...routers], s.hot, (v) => { s.hot = v; refresh(); }, "The throw when an unblocked rusher comes"))),
    );
  } else {
    const r = p.run;
    const carriers = f.slots.filter((s) => O.roleOf(s.pos) !== "ol" && !O.isPasser(s)).map((s) => s.name);
    const types = [...new Set(r.path.map((pt) => pt.type).filter(Boolean))];
    play.append(
      row(field("scheme", select(keys(lib.blocking.run_schemes), r.scheme, (v) => { r.scheme = v; refresh(); })),
        field("direction", segmented(["left", "right"], r.direction, (v) => { r.direction = v as Side; renderAll(); }))),
      row(field("carrier", select(carriers, r.carrier, (v) => { r.carrier = v; renderAll(); })),
        field("QB then", select([["", "(nothing)"], ...keys(lib.qb.fakes)], r.then, (v) => { r.then = v; refresh(); }, "What the QB does after the handoff"))),
      row(field("reads", chips(O.RUN_READS, r.reads, (v) => { r.reads = v; refresh(); }))),
      row(field("free after", select([["", "(the end)"], ...types], r.freeAfter, (v) => { r.freeAfter = v; refresh(); }, "The carrier runs free after the path point of this type"))),
      el("div", { className: "sub" }, el("span", { className: "lbl", textContent: "path (from the carrier; drag the green points)" }), pathEditor()),
    );
  }
  play.append(tagsBox(O.OFFENSE_TAGS, p.tags, (v) => { p.tags = v; refresh(); }));
  const form = section("Formation",
    row(field("id", textInput(f.id, (v) => { f.id = v; refresh(); })), field("name", textInput(f.name, (v) => { f.name = v; refresh(); }))),
    row(field("personnel", textInput(f.personnel, (v) => { f.personnel = v; refresh(); }, "RBs then TEs (11 = 1 back, 1 TE). Empty counts them", O.personnelOf(f)))),
  );
  return [play, form];
}

function pathEditor(): HTMLElement {
  const r = state.off.play.run;
  const box = el("div", { className: "path" });
  r.path.forEach((pt, i) => {
    box.append(el("div", { className: "path-pt" },
      el("b", { textContent: `${i + 1}` }),
      anchorEditor(pt.x, lay.path[i]?.x ?? 0, (a) => { pt.x = a; refresh(); }, { defense: false, gaps: true, slotNames: () => state.off.formation.slots.map((s) => s.name) }),
      field("y", numberInput(pt.y, (v) => { pt.y = v ?? 0; refresh(); }, 0.5)),
      select([["", "(none)"], ...O.PATH_TYPES], pt.type, (v) => { pt.type = v; renderAll(); }, "mesh: where he takes the handoff; aim: where he's going"),
      button("×", () => { r.path.splice(i, 1); renderAll(); }, "Remove this point", "mini"),
    ));
  });
  box.append(button("+ point", () => { const last = r.path[r.path.length - 1]; r.path.push({ x: { kind: "number", value: lay.path[r.path.length - 1]?.x ?? 0 }, y: (last?.y ?? -4) + 2, type: "" }); renderAll(); }, "", "mini"));
  return box;
}

function tagsBox(all: readonly string[], tags: string[], on: (v: string[]) => void): HTMLElement {
  const d = el("details", {}, el("summary", { textContent: `tags (${tags.length})${tags.length ? `: ${tags.join(", ")}` : ""}` }), chips(all, tags, (v) => { on(v); d.querySelector("summary")!.textContent = `tags (${v.length})${v.length ? `: ${v.join(", ")}` : ""}`; }));
  return d;
}

function defenseLeft(): HTMLElement[] {
  const d = state.def;
  const p = d.play;
  const play = section("Play",
    row(field("id", textInput(p.id, (v) => { p.id = v; refresh(); })), field("name", textInput(p.name, (v) => { p.name = v; refresh(); }))),
    row(field("coverage", select([["", "(none)"], ...D.COVERAGES], p.coverage, (v) => { p.coverage = v; refresh(); }))),
    row(field("show", segmented([["pass", "pass jobs"], ["run", "run jobs"]], d.view, (v) => { d.view = v as "pass" | "run"; renderAll(); }))),
    tagsBox(D.DEFENSE_TAGS, p.tags, (v) => { p.tags = v; refresh(); }),
  );
  const book = section("Strength (playbook)",
    el("div", { className: "sub" }, el("span", { className: "lbl", textContent: "strength_call: tried in order, the first that picks a side wins" }),
      orderedList(d.calls, D.STRENGTH_CALLS.filter((c) => !d.calls.includes(c)), (v) => { d.calls = v; renderAll(); })),
    row(field("preview", segmented([["", "from the calls"], ["left", "strong left"], ["right", "strong right"]], d.override, (v) => { d.override = v as Side | ""; renderAll(); }))),
    note("The preview only forces the strong side here, to check the other look; it isn't written to the play."),
  );
  const f = d.formation;
  const form = section("Formation",
    row(field("id", textInput(f.id, (v) => { f.id = v; refresh(); })), field("name", textInput(f.name, (v) => { f.name = v; refresh(); }))),
    row(field("package", select(D.PACKAGES, f.package, (v) => { f.package = v; refresh(); }))),
    note(`Lines up against the formation on the Offense tab (${state.off.formation.name || state.off.formation.id}). Import another one there to check this front against it.`),
  );
  return [play, book, form];
}

// ---------------------------------------------------------------------------
// The right panel: the selected player
// ---------------------------------------------------------------------------

function renderInspector(): void {
  const box = $("right");
  const name = state.selected[state.mode];
  if (state.mode === "offense") {
    const s = offSlot(name);
    box.replaceChildren(...(s ? offenseInspector(s) : [emptyInspector()]));
  } else {
    const s = defSlot(name);
    box.replaceChildren(...(s ? defenseInspector(s) : [emptyInspector()]));
  }
}

function emptyInspector(): HTMLElement {
  return section("No player selected", note("Click a player on the field or in the Players list to edit his alignment and his job. Drag players to move them: a number position changes x and y; an anchored one keeps its landmark and changes its offset."));
}

function inspectorHead(name: string, pos: string, remove: () => void): HTMLElement {
  return el("div", { className: "insp-head" }, el("strong", { textContent: name }), el("span", { className: "muted", textContent: pos }), el("span", { className: "spacer" }),
    button("Remove", remove, "Take this slot out of the formation", "danger"), button("✕", () => selectSlot(""), "Close", "mini"));
}

// The name box renames on change (not every keystroke).
function nameInput(name: string, rename: (to: string) => void): HTMLInputElement {
  const i = el("input", { type: "text", value: name, spellcheck: false, className: "short" });
  i.addEventListener("change", () => rename(i.value.trim()));
  return i;
}

// ---- offense

function offenseInspector(s: O.OffSlot): HTMLElement[] {
  const { formation: f, play: p } = state.off;
  const others = () => f.slots.filter((o) => o.name !== s.name).map((o) => o.name);
  const at = lay.off[s.name] ?? { x: 0, y: s.y };
  const align = section("Alignment (formation)",
    row(field("slot", nameInput(s.name, (to) => renameOff(s.name, to))), field("position", select(O.OFFENSE_POSITIONS, s.pos, (v) => { s.pos = v; renderAll(); }))),
    row(field("x", anchorEditor(s.x, at.x, (a) => { s.x = a; refresh(); }, { defense: false, gaps: false, slotNames: others }))),
    row(field("y", numberInput(s.y, (v) => { s.y = v ?? 0; refresh(); }, 0.5, "Yards from the line; behind it is negative")),
      el("span", { className: "muted", textContent: `at ${at.x.toFixed(2)}, ${at.y}` })),
    row(field("stance", select([["", "(none)"], ...O.STANCES], s.stance, (v) => { s.stance = v; renderAll(); }, "snap: the center; gun, under_center, pistol: the passer")),
      field("on the line", checkbox(s.onLine, (c) => { s.onLine = c; refresh(); }, "An offense needs at least 7 on the line"))),
  );
  return [inspectorHead(s.name, s.pos, () => removeOff(s.name)), align, assignmentSection(s, p)];
}

function assignmentSection(s: O.OffSlot, p: O.OffensePlay): HTMLElement {
  const map = O.currentAssignments(p);
  const a = map[s.name] ?? { kind: "none" as const };
  const cur = O.choiceOf(a);
  const options = O.choicesFor(s, p);
  const set = (next: O.OffAssign) => { map[s.name] = next; renderAll(); };
  const sec = section(`Assignment (${p.type} play)`);
  if (options.length > 1 || !options.includes(cur)) {
    sec.append(row(select([...new Set([...options, cur])].map((c): [string, string] => [c, O.CHOICE_LABELS[c]]), cur, (c) => set(O.makeAssign(c as O.Choice, s, p.type, offSide(s.name), keys(lib.routes))))));
  }
  if (!O.legal(a, s, p)) sec.append(el("p", { className: "bad note", textContent: "The loader won't take this here; pick another job." }));
  switch (cur) {
    case "none":
      sec.append(note(O.isCarrier(s, p) ? "The ball carrier: his path is in the Play panel, and he needs no other job." : O.isPasser(s) ? "The passer: his drop (pass) or fake (run) is in the Play panel." : "No job."));
      break;
    case "protect":
      sec.append(note(`Follows the play's protection (${p.pass.protection}${p.pass.direction ? `, sliding ${p.pass.direction}` : ""}).`));
      break;
    case "scheme":
      sec.append(note(`Follows the ${p.run.scheme} rules in data/library/blocking_schemes.json.`));
      break;
    case "route":
      if (a.kind === "route") sec.append(routeEditor(a.route, O.roleOf(s.pos) === "back"));
      break;
    case "release":
      if (a.kind === "block" && a.then) {
        sec.append(note("Checks for a free rusher first, then releases into his route."), routeEditor(a.then, true));
      }
      break;
    case "block":
      if (a.kind === "block") {
        const b = a.block;
        sec.append(row(
          field("block", select(O.BLOCK_ACTIONS[p.type], b.action, (v) => { b.action = v; b.target = O.USUAL_TARGET[v] ?? b.target; renderAll(); })),
          field("target", select([["", "(none)"], ...O.BLOCK_TARGETS], b.target, (v) => { b.target = v; refresh(); })),
        ));
      }
      break;
  }
  if (p.type === "pass" && O.runsRoute(map[s.name])) {
    const prog = p.pass.progression;
    const i = prog.indexOf(s.name);
    sec.append(row(
      i >= 0
        ? el("span", {}, el("span", { textContent: `Read #${i + 1} of ${prog.length}  ` }), button("take out of the progression", () => { prog.splice(i, 1); renderAll(); }, "", "mini"))
        : button("+ add to the progression", () => { prog.push(s.name); renderAll(); }, "", "mini"),
      field("hot", checkbox(p.pass.hot === s.name, (c) => { p.pass.hot = c ? s.name : ""; renderAll(); }, "The throw when an unblocked rusher comes")),
    ));
  }
  return sec;
}

// A route: the picker, its depth, which way "outside" is, and options.
function routeEditor(r: O.RouteJob, isBack: boolean): HTMLElement {
  const box = el("div", { className: "route-ed" });
  const grid = el("div", { className: "route-grid" });
  for (const name of keys(lib.routes)) {
    grid.append(button(name, () => { r.route = name; delete r.depth; renderAll(); }, "", name === r.route ? "on" : ""));
  }
  const tmpl = lib.routes[r.route];
  const brk = tmpl?.points.find((pt) => pt.type === "break");
  const sideTitle = "Which sideline is 'outside' for the route. Auto: the side he lines up on" + (isBack ? " (set it for a back: his release side)" : "");
  box.append(grid, row(
    field("depth", numberInput(r.depth, (v) => { if (v === undefined) delete r.depth; else r.depth = v; refresh(); }, 1, "Moves the route from its first break; empty keeps the template", brk ? `${brk.y}` : "—")),
    field("outside is", select([["", "auto"], "left", "right"], r.side ?? "", (v) => { if (v) r.side = v as Side; else delete r.side; refresh(); }, sideTitle)),
  ));
  const option = (label: string, get: () => O.RouteRef | undefined, set: (v: O.RouteRef | undefined) => void) => {
    const o = get();
    return row(
      field(label, checkbox(!!o, (c) => { set(c ? { route: keys(lib.routes).find((n) => n !== r.route) ?? r.route } : undefined); renderAll(); })),
      ...(o ? [select(keys(lib.routes), o.route, (v) => { o.route = v; refresh(); }), field("depth", numberInput(o.depth, (v) => { if (v === undefined) delete o.depth; else o.depth = v; refresh(); }, 1, "", "template"))] : []),
    );
  };
  box.append(
    option("vs man", () => r.vsMan, (v) => { if (v) r.vsMan = v; else delete r.vsMan; }),
    option("vs zone", () => r.vsZone, (v) => { if (v) r.vsZone = v; else delete r.vsZone; }),
  );
  if (r.depth !== undefined && tmpl) {
    const last = routePoints(tmpl, r.depth).at(-1);
    if (last && last.y < 0) box.append(el("p", { className: "bad note", textContent: "That depth puts the route's end behind where he lines up." }));
  }
  return box;
}

function renameOff(from: string, to: string): void {
  const f = state.off.formation;
  if (!to || to === from) return renderAll();
  if (f.slots.some((s) => s.name === to)) {
    say(`There's already a slot named ${to}.`, true);
    return renderAll();
  }
  const fix = (a: Anchor): Anchor => (a.kind === "slot" && a.ref === from ? { ...a, ref: to } : a);
  for (const s of f.slots) {
    if (s.name === from) s.name = to;
    s.x = fix(s.x);
  }
  const p = state.off.play;
  for (const map of Object.values(p.assignments)) {
    if (map[from]) {
      map[to] = map[from]!;
      delete map[from];
    }
  }
  p.pass.progression = p.pass.progression.map((n) => (n === from ? to : n));
  if (p.pass.hot === from) p.pass.hot = to;
  if (p.run.carrier === from) p.run.carrier = to;
  for (const pt of p.run.path) pt.x = fix(pt.x);
  state.selected.offense = to;
  renderAll();
}

// Removing a slot: anything anchored to him is pinned where it stands now.
function removeOff(name: string): void {
  const f = state.off.formation;
  const pin = (a: Anchor, x: number | undefined): Anchor => (a.kind === "slot" && a.ref === name ? { kind: "number", value: Math.round((x ?? 0) * 100) / 100 } : a);
  for (const s of f.slots) s.x = pin(s.x, lay.off[s.name]?.x);
  state.off.play.run.path.forEach((pt, i) => (pt.x = pin(pt.x, lay.path[i]?.x)));
  f.slots = f.slots.filter((s) => s.name !== name);
  state.selected.offense = "";
  renderAll();
}

// ---- defense

function defenseInspector(s: D.DefSlot): HTMLElement[] {
  const d = state.def;
  const a = d.play.assignments[s.name]!;
  const role = D.defRoleOf(s.pos);
  const at = lay.def[s.name] ?? { x: 0, y: s.y };
  const opts = { defense: true, gaps: false, slotNames: () => d.formation.slots.filter((o) => o.name !== s.name).map((o) => o.name) };
  const hasAlign = a.align.x !== undefined || a.align.y !== undefined;
  const align = section("Alignment (formation)",
    row(field("slot", nameInput(s.name, (to) => renameDef(s.name, to))), field("position", select(D.DEFENSE_POSITIONS, s.pos, (v) => { s.pos = v; renderAll(); }))),
    row(field("x", anchorEditor(s.x, at.x, (v) => { s.x = v; refresh(); }, opts))),
    row(field("y", numberInput(s.y, (v) => { s.y = v ?? 0; refresh(); }, 0.5, "Yards off the ball")),
      field("stance", select([["", "(none)"], "three_point", "two_point"], s.stance, (v) => { s.stance = v; refresh(); }))),
    ...(hasAlign ? [note("This call moves him (below), so dragging changes the call's alignment, not the formation's.")] : []),
  );
  const callAlign = section("This call's alignment (align)",
    row(
      field("x", checkbox(a.align.x !== undefined, (c) => { if (c) a.align.x = s.x; else delete a.align.x; renderAll(); }, "Replace the formation's x for this play")),
      ...(a.align.x !== undefined ? [anchorEditor(a.align.x, at.x, (v) => { a.align.x = v; refresh(); }, opts)] : []),
    ),
    row(
      field("y", checkbox(a.align.y !== undefined, (c) => { if (c) a.align.y = s.y; else delete a.align.y; renderAll(); }, "Replace the formation's y for this play")),
      ...(a.align.y !== undefined ? [numberInput(a.align.y, (v) => { a.align.y = v ?? 0; refresh(); })] : []),
    ),
    note("Cover 2 splits the safeties over the hashes; man defenders line up over their man. A man defender's cushion starts at his depth."),
  );
  return [inspectorHead(s.name, s.pos, () => removeDef(s.name)), align, runSection(s, a, role), passSection(s, a, role), callAlign];
}

function runSection(s: D.DefSlot, a: D.DefAssign, role: D.DefRole): HTMLElement {
  const j = a.run;
  const kinds = [...new Set([...D.RUN_MENU[role], j.kind])];
  const sec = section("Run job", row(select(kinds.map((k): [string, string] => [k, D.RUN_LABELS[k]]), j.kind, (k) => { a.run = D.makeRun(k as D.RunKind, s, defSide(s.name)); renderAll(); })));
  if (j.kind === "gap") sec.lastElementChild!.append(select(GAPS, j.gap, (g) => { j.gap = g as Gap; refresh(); }), select(SIDE_WORDS, j.side, (v) => { j.side = v as SideWord; refresh(); }));
  if (j.kind === "support") sec.lastElementChild!.append(select(D.SUPPORTS, j.ref, (v) => { j.ref = v; refresh(); }, "alley / force: a spot outside the end of the line; deep_third: keep his deep zone; deep_fill: behind the line in the middle"));
  if (j.kind === "contain") sec.append(note("Sets the edge on his side, outside the end of the line."));
  if (j.kind === "pursue") sec.append(note("Chases the ball."));
  return sec;
}

function passSection(s: D.DefSlot, a: D.DefAssign, role: D.DefRole): HTMLElement {
  const j = a.pass;
  const kinds = [...new Set([...D.PASS_MENU[role], j.kind])];
  const r = row(select(kinds.map((k): [string, string] => [k, D.PASS_LABELS[k]]), j.kind, (k) => { a.pass = D.makePass(k as D.PassKind, s, defSide(s.name), keys(lib.zones)); renderAll(); }));
  const sec = section("Pass job", r);
  const side = (v: SideWord | "", own: boolean, on: (v: string) => void) => select(own ? [["", "his side"], ...SIDE_WORDS] : SIDE_WORDS, v, on);
  switch (j.kind) {
    case "rush":
      r.append(select(D.RUSH_LANES, j.lane, (v) => { j.lane = v; refresh(); }, "A gap, or contain (outside the end of the line)"), side(j.side, true, (v) => { j.side = v as SideWord | ""; refresh(); }));
      break;
    case "zone":
      r.append(field("Zone", select(keys(lib.zones), j.ref, (v) => { j.ref = v; refresh(); })));
      sec.append(field("Zone read", select([["", "Automatic"], ...D.MAN_TARGETS], j.read ?? "", (v) => {
        if (v) j.read = v; else delete j.read;
        refresh();
      }, "Deep zones only: carry this read when it goes vertical. Automatic uses the zone's normal threat selection.")));
      sec.append(note("An optional read applies to deep zones only."));
      break;
    case "man":
      r.append(select(D.MAN_TARGETS, j.target, (v) => { j.target = v; refresh(); }, "Receivers are numbered from the sideline in on each side; back_1 is the shallowest back"),
        select(["inside", "outside"], j.leverage, (v) => { j.leverage = v as "inside" | "outside"; refresh(); }, "Which shoulder he plays"));
      break;
    case "blitz":
      r.append(select(GAPS, j.gap, (g) => { j.gap = g as Gap; refresh(); }), side(j.side, false, (v) => { j.side = v as SideWord; refresh(); }));
      break;
    case "stunt":
      r.append(select(D.RUSH_LANES, j.lane, (v) => { j.lane = v; refresh(); }));
      sec.append(note("Stunts are plain rushes in the sim for now."));
      break;
    case "spy":
      sec.append(note("Sits 5 yd off the ball on the QB."));
      break;
  }
  return sec;
}

function renameDef(from: string, to: string): void {
  const d = state.def;
  if (!to || to === from) return renderAll();
  if (d.formation.slots.some((s) => s.name === to)) {
    say(`There's already a slot named ${to}.`, true);
    return renderAll();
  }
  const fix = (a: Anchor): Anchor => (a.kind === "slot" && a.ref === from ? { ...a, ref: to } : a);
  for (const s of d.formation.slots) {
    if (s.name === from) s.name = to;
    s.x = fix(s.x);
  }
  const a = d.play.assignments;
  if (a[from]) {
    a[to] = a[from]!;
    delete a[from];
  }
  for (const x of Object.values(a)) if (x.align.x) x.align.x = fix(x.align.x);
  state.selected.defense = to;
  renderAll();
}

function removeDef(name: string): void {
  const d = state.def;
  const pin = (a: Anchor, x: number | undefined): Anchor => (a.kind === "slot" && a.ref === name ? { kind: "number", value: Math.round((x ?? 0) * 100) / 100 } : a);
  for (const s of d.formation.slots) {
    s.x = pin(s.x, lay.def[s.name]?.x);
    const al = d.play.assignments[s.name]?.align;
    if (al?.x) al.x = pin(al.x, lay.def[s.name]?.x);
  }
  d.formation.slots = d.formation.slots.filter((s) => s.name !== name);
  state.selected.defense = "";
  renderAll();
}

function addSlot(pos: string): void {
  if (!pos) return;
  if (state.mode === "offense") {
    const s = O.newSlot(pos, state.off.formation.slots.map((x) => x.name));
    state.off.formation.slots.push(s);
    state.selected.offense = s.name;
  } else {
    const s = D.newDefSlot(pos, state.def.formation.slots.map((x) => x.name));
    state.def.formation.slots.push(s);
    state.selected.defense = s.name;
  }
  renderAll();
}

// ---------------------------------------------------------------------------
// Dragging on the field
// ---------------------------------------------------------------------------

interface Drag { slot?: string; path?: number; start: { x: number; y: number }; moved: boolean }
let drag: Drag | null = null;
const svg = $<SVGSVGElement>("field");

function toField(e: PointerEvent): { x: number; y: number } {
  const m = svg.getScreenCTM();
  if (!m) return { x: 0, y: 0 };
  const p = new DOMPoint(e.clientX, e.clientY).matrixTransform(m.inverse());
  return { x: p.x, y: -p.y };
}

svg.addEventListener("pointerdown", (e) => {
  const t = (e.target as Element).closest("[data-slot],[data-path]");
  if (!t) {
    if (state.selected[state.mode]) selectSlot("");
    return;
  }
  const slot = t.getAttribute("data-slot") ?? undefined;
  const path = t.hasAttribute("data-path") ? Number(t.getAttribute("data-path")) : undefined;
  drag = { slot, path, start: toField(e), moved: false };
  svg.setPointerCapture(e.pointerId);
  if (slot && state.selected[state.mode] !== slot) selectSlot(slot);
  e.preventDefault();
});

svg.addEventListener("pointermove", (e) => {
  if (!drag) return;
  const p = toField(e);
  if (!drag.moved && Math.hypot(p.x - drag.start.x, p.y - drag.start.y) < 0.3) return;
  drag.moved = true;
  const step = e.shiftKey ? 0.1 : 0.5;
  const snapY = (y: number) => snap(y, step);
  if (state.mode === "offense") {
    const ctx: Ctx = { frame: lay.frame, read: lay.read, slotPos: (n) => lay.off[n] };
    if (drag.path !== undefined) {
      const pt = state.off.play.run.path[drag.path];
      if (!pt) return;
      pt.x = withX(pt.x, p.x, ctx, step);
      pt.y = snapY(p.y);
    } else if (drag.slot) {
      const s = offSlot(drag.slot);
      if (!s) return;
      s.x = withX(s.x, p.x, ctx, step);
      s.y = snapY(p.y);
    }
  } else if (drag.slot && lay.read) {
    const s = defSlot(drag.slot);
    const a = s && state.def.play.assignments[s.name];
    if (!s || !a) return;
    const ctx: Ctx = { frame: lay.frame, read: lay.read, slotPos: (n) => lay.def[n] };
    if (a.align.x !== undefined) a.align.x = withX(a.align.x, p.x, ctx, step);
    else s.x = withX(s.x, p.x, ctx, step);
    if (a.align.y !== undefined) a.align.y = snapY(p.y);
    else s.y = snapY(p.y);
  }
  refresh();
});

const endDrag = () => {
  if (drag?.moved) renderAll(); // the panels show the new numbers
  drag = null;
};
svg.addEventListener("pointerup", endDrag);
svg.addEventListener("pointercancel", endDrag);

// ---------------------------------------------------------------------------
// Import
// ---------------------------------------------------------------------------

const dialog = $<HTMLDialogElement>("import-dialog");

function importPasted(p: Pasted): void {
  const result = $("import-result");
  if (p.kind === "playbook") {
    if (p.side === "defense" && p.strengthCall.length) {
      state.def.calls = p.strengthCall;
    }
    result.replaceChildren(
      el("p", { textContent: `Playbook ${p.id}: pick a play.${p.side === "defense" && p.strengthCall.length ? ` Its strength call (${p.strengthCall.join(", ")}) was taken.` : ""}` }),
      el("div", { className: "pick" }, ...p.plays.map((play) => button(`${String(play.name ?? play.id)}  ·  ${String(play.formation ?? "")}`, () => importPasted({ kind: "play", side: p.side, json: play }), String(play.id ?? "")))),
    );
    renderAll();
    return;
  }
  const warnings: string[] = [];
  if (p.side === "offense") {
    if (p.kind === "formation") {
      const f = O.parseOffenseFormation(p.json);
      state.off.formation = f;
      state.off.baseline = offBaseline(f);
    } else {
      const r = O.parseOffensePlay(p.json, state.off.formation);
      state.off.play = r.play;
      warnings.push(...r.warnings);
    }
  } else if (p.kind === "formation") {
    const f = D.parseDefenseFormation(p.json);
    state.def.formation = f;
    state.def.baseline = defBaseline(f);
  } else {
    const r = D.parseDefensePlay(p.json, state.def.formation);
    state.def.play = r.play;
    warnings.push(...r.warnings);
  }
  state.mode = p.side;
  state.selected[p.side] = "";
  const what = `${p.side} ${p.kind} '${String((p.json as Obj).id ?? "")}'`;
  say(warnings.length ? `Imported the ${what}, with notes: ${warnings.join("; ")}` : `Imported the ${what}.`, warnings.length > 0);
  dialog.close();
  renderAll();
}

$("btn-import").addEventListener("click", () => {
  $("import-result").replaceChildren();
  dialog.showModal();
  $<HTMLTextAreaElement>("import-text").select();
});
$("import-close").addEventListener("click", () => dialog.close());
$("import-go").addEventListener("click", () => {
  try {
    importPasted(readPasted($<HTMLTextAreaElement>("import-text").value));
  } catch (e) {
    $("import-result").replaceChildren(el("p", { className: "bad", textContent: (e as Error).message }));
  }
});

// ---------------------------------------------------------------------------
// The top bar
// ---------------------------------------------------------------------------

function setMode(mode: Mode): void {
  state.mode = mode;
  renderAll();
}
$("btn-offense").addEventListener("click", () => setMode("offense"));
$("btn-defense").addEventListener("click", () => setMode("defense"));
$<HTMLSelectElement>("hash").addEventListener("change", (e) => {
  state.hash = (e.target as HTMLSelectElement).value as Hash;
  renderAll();
});
$("btn-new-play").addEventListener("click", () => {
  if (!confirm(`Start a new ${state.mode} play in this formation? The current play's assignments are cleared.`)) return;
  if (state.mode === "offense") state.off.play = O.defaultOffensePlay();
  else state.def.play = { id: "new_defense", name: "New Defense", coverage: "", tags: [], assignments: {} };
  notice = null;
  renderAll();
});
$("btn-reset").addEventListener("click", () => {
  if (!confirm(`Reset the ${state.mode} to the starting formation and play? This can't be undone.`)) return;
  if (state.mode === "offense") state.off = freshOff();
  else state.def = freshDef();
  state.selected[state.mode] = "";
  notice = null;
  renderAll();
});

// ---------------------------------------------------------------------------

function safeRender(): void {
  try {
    renderAll();
  } catch (e) {
    // A saved draft this version can't show: start over rather than show nothing.
    console.error(e);
    state = freshState();
    say(`The saved draft couldn't be opened (${(e as Error).message}); started fresh.`, true);
    renderAll();
  }
}

async function start(): Promise<void> {
  try {
    lib = (await (await fetch("/api/library")).json()) as Library;
  } catch (e) {
    say(`Couldn't load data/library/ (run the viewer's dev server): ${String(e)}`, true);
  }
  safeRender();
}
void start();
