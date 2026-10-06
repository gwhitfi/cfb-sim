// Small form controls for the play builder page, and the anchor editor.

import { el } from "../builder/dom";
import { GAPS, RECEIVERS, SIDE_WORDS, TECHNIQUES, num, type Anchor, type AnchorKind, type Gap, type SideWord } from "./anchors";

export type Option = string | [value: string, label: string];
const asPair = (o: Option): [string, string] => (typeof o === "string" ? [o, o] : o);

// A select. A value missing from the options is added (marked), so loaded data
// is never silently changed by the control.
export function select(options: readonly Option[], value: string, on: (v: string) => void, title = ""): HTMLSelectElement {
  const s = el("select", { title });
  const pairs = options.map(asPair);
  if (!pairs.some(([v]) => v === value)) pairs.push([value, `${value || "(none)"} *`]);
  for (const [v, label] of pairs) s.append(el("option", { value: v, textContent: label }));
  s.value = value;
  s.addEventListener("change", () => on(s.value));
  return s;
}

// A number box. Calls `on` with a number, or undefined when emptied; text that
// isn't a number yet ("-", "1.") is ignored until it is.
export function numberInput(value: number | undefined, on: (v: number | undefined) => void, step = 0.5, title = "", placeholder = ""): HTMLInputElement {
  const i = el("input", { type: "number", step, value: value === undefined ? "" : String(value), title, placeholder, className: "num" });
  i.addEventListener("input", () => {
    if (i.value === "") on(undefined);
    else if (!Number.isNaN(i.valueAsNumber)) on(i.valueAsNumber);
  });
  return i;
}

export function textInput(value: string, on: (v: string) => void, title = "", placeholder = ""): HTMLInputElement {
  const i = el("input", { type: "text", value, title, placeholder, spellcheck: false });
  i.addEventListener("input", () => on(i.value));
  return i;
}

export function checkbox(checked: boolean, on: (v: boolean) => void, title = ""): HTMLInputElement {
  const c = el("input", { type: "checkbox", checked, title });
  c.addEventListener("change", () => on(c.checked));
  return c;
}

export function field(label: string, ...controls: (Node | string)[]): HTMLElement {
  return el("label", { className: "field" }, el("span", { className: "lbl", textContent: label }), ...controls);
}

export function button(text: string, onclick: () => void, title = "", className = ""): HTMLButtonElement {
  return el("button", { type: "button", textContent: text, title, className, onclick });
}

// Toggle buttons for a set of values; `on` gets the chosen ones in the list's order.
export function chips(options: readonly string[], selected: readonly string[], on: (v: string[]) => void): HTMLElement {
  const box = el("div", { className: "chips" });
  const chosen = new Set(selected);
  for (const o of options) {
    const b = button(o, () => {
      if (chosen.has(o)) chosen.delete(o);
      else chosen.add(o);
      b.classList.toggle("on", chosen.has(o));
      on(options.filter((x) => chosen.has(x)));
    }, "", `chip${chosen.has(o) ? " on" : ""}`);
    box.append(b);
  }
  return box;
}

// One choice from a few, as a row of buttons.
export function segmented(options: readonly Option[], value: string, on: (v: string) => void): HTMLElement {
  const box = el("div", { className: "seg" });
  for (const o of options) {
    const [v, label] = asPair(o);
    box.append(button(label, () => on(v), "", v === value ? "on" : ""));
  }
  return box;
}

// An ordered list (the progression, the strength call): each item can move up
// or be removed, and `add` lists what can be appended.
export function orderedList(items: readonly string[], add: readonly string[], on: (v: string[]) => void, numbered = true): HTMLElement {
  const box = el("div", { className: "ordered" });
  const list = el("div", { className: "ordered-items" });
  items.forEach((it, i) => {
    list.append(el("span", { className: "ord" },
      ...(numbered ? [el("b", { textContent: `${i + 1}` })] : []),
      el("span", { textContent: it }),
      ...(i > 0 ? [button("↑", () => on([...items.slice(0, i - 1), it, items[i - 1]!, ...items.slice(i + 1)]), "Move earlier", "mini")] : []),
      button("×", () => on(items.filter((_, j) => j !== i)), "Remove", "mini"),
    ));
  });
  if (items.length === 0) list.append(el("span", { className: "muted", textContent: "(empty)" }));
  box.append(list);
  if (add.length) box.append(el("div", { className: "ordered-add" }, el("span", { className: "muted", textContent: "add" }), ...add.map((a) => button(`+ ${a}`, () => on([...items, a]), "", "mini"))));
  return box;
}

// ---- the anchor editor

export interface AnchorOptions {
  defense: boolean; // tech and over anchors, and strong/weak/field/boundary sides
  gaps: boolean; // gap anchors (run paths)
  slotNames: () => string[]; // slots a "from slot" anchor can use
}

// The loader allows relative sides only in defensive files.
const sidesFor = (defense: boolean): SideWord[] => (defense ? [...SIDE_WORDS] : ["left", "right"]);

function fresh(kind: AnchorKind, prev: Anchor, x: number, o: AnchorOptions): Anchor {
  const side: SideWord = o.defense ? "strong" : x < 0 ? "left" : "right";
  const offset = "offset" in prev ? prev.offset : 0;
  switch (kind) {
    case "number": return num(Math.round(x * 2) / 2);
    case "ball": return { kind, offset };
    case "numbers":
    case "hash":
    case "sideline": return { kind, side, offset: 0 };
    case "slot": return { kind, ref: o.slotNames()[0] ?? "C", offset: 0 };
    case "gap": return { kind, ref: "B", side: o.defense ? "strong" : side, offset: 0 };
    case "tech": return { kind, ref: "3", side, offset: 0 };
    case "over": return { kind, ref: "#1", side, offset: 0 };
  }
}

const LABELS: Record<AnchorKind, string> = { number: "yards from ball", ball: "the ball", numbers: "the numbers", hash: "the hash", sideline: "the sideline", slot: "another slot", gap: "a gap", tech: "a technique", over: "over a receiver" };
const OFFSET_TITLE = "Offset in yards. With left/right (or no side) + is to the right; with strong/weak/field/boundary + is toward that sideline.";

// Edits an anchor: "yards from ball" is a plain number; the rest are
// landmarks plus an offset. `x` is where it resolves now, so switching kinds
// starts close to the current spot. Every change calls `on`.
export function anchorEditor(start: Anchor, x: number, on: (a: Anchor) => void, o: AnchorOptions): HTMLElement {
  const box = el("span", { className: "anchor" });
  let cur = start;
  const set = (a: Anchor) => {
    cur = a;
    on(a);
  };
  const render = () => {
    const kinds: AnchorKind[] = ["number", "ball", "numbers", "hash", "sideline", "slot"];
    if (o.gaps) kinds.push("gap");
    if (o.defense) kinds.push("tech", "over");
    const a = cur;
    const parts: HTMLElement[] = [
      select(kinds.map((k): [string, string] => [k, LABELS[k]]), a.kind, (k) => {
        set(fresh(k as AnchorKind, cur, x, o));
        render();
      }, "What x is measured from"),
    ];
    if (a.kind === "number") parts.push(numberInput(a.value, (v) => set({ kind: "number", value: v ?? 0 }), 0.5, "Yards right of the ball (left is negative)"));
    if (a.kind === "slot") parts.push(select(o.slotNames(), a.ref, (r) => set({ ...a, ref: r }), "Measured from where this slot stands"));
    if (a.kind === "gap") parts.push(select(GAPS, a.ref, (r) => set({ ...a, ref: r as Gap })));
    if (a.kind === "tech") parts.push(select(TECHNIQUES, a.ref, (r) => set({ ...a, ref: r }), "0 head up on the C, 1 the A gap, 2i/2/3 on the G, 4i/4/5 on the T, 7/6/9 on the TE"));
    if (a.kind === "over") parts.push(select(RECEIVERS, a.ref, (r) => set({ ...a, ref: r }), "Receivers are numbered from the sideline in"));
    if ("side" in a) parts.push(select(sidesFor(o.defense), a.side, (s) => set({ ...a, side: s as SideWord } as Anchor)));
    if (a.kind !== "number") parts.push(numberInput(a.offset, (v) => set({ ...a, offset: v ?? 0 }), 0.5, OFFSET_TITLE));
    box.replaceChildren(...parts);
  };
  render();
  return box;
}
