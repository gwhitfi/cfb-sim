// The play menu: the offense's call as pictures, Madden style
// (docs/PROPOSALS.md#the-play-menu). Formations first (players only), then a
// formation's plays (routes, the first read's in red), each with its name.
// The pictures come from the server's playbook art (art.ts).

import { formationSvg, playSvg, type FormationArt, type PlayArt, type PlaybookArt } from "./art";
import { CHOICE_NAMES, type PlayOption } from "./game";

// One formation and the plays the menu shows in it, in the playbook's order.
export interface MenuGroup {
  formation: FormationArt;
  plays: PlayArt[];
}

// The formations and plays to show. `offered`: the call's plays (the
// prompt's options), or null before the call is open (every play in the
// art). A play with no art is left out; a formation with no play shown too.
export function menuGroups(art: PlaybookArt, offered: PlayOption[] | null): MenuGroup[] {
  const ids = offered ? new Set(offered.map((p) => p.id)) : null;
  const out: MenuGroup[] = [];
  for (const formation of art.formations) {
    const plays = art.plays.filter((p) => p.formation === formation.id && (!ids || ids.has(p.id)));
    if (plays.length) out.push({ formation, plays });
  }
  return out;
}

// The call's plays that have no picture (the menu lists them by name so
// none is lost).
export function missingArt(art: PlaybookArt, offered: PlayOption[] | null): PlayOption[] {
  if (!offered) return [];
  const ids = new Set(art.plays.map((p) => p.id));
  return offered.filter((p) => !ids.has(p.id));
}

// What the menu needs for one call.
export interface MenuCall {
  art: PlaybookArt;
  plays: PlayOption[] | null; // the prompt's plays; null before the call is open
  suggestion?: { play?: string; flip?: boolean } | null;
  kicks: string[]; // punt and field goal on 4th down, the extra point on a try
  clock: string[]; // kneel, spike
}

// The answer a pick makes: {play, flip}, a kick or clock play {play}, or the coach's call {auto}.
export type MenuAnswer = Record<string, unknown>;

const h = <K extends keyof HTMLElementTagNameMap>(tag: K, cls = "", text = ""): HTMLElementTagNameMap[K] => {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text) e.textContent = text;
  return e;
};

// The menu's own state (which screen, flip) lasts from the whistle to the
// call, across re-renders as the call's options arrive.
export class PlayMenu {
  formation: string | null = null; // the plays screen for this formation; null: the formations
  flip = false;
  // What a pick does: answer the call, or hold the pick until the call opens.
  onPick: (answer: MenuAnswer) => void = () => {};

  // Back from the plays screen. False if already on the formations.
  back(): boolean {
    if (this.formation === null) return false;
    this.formation = null;
    return true;
  }

  // Builds the menu into `box` (its tools row, then the cards).
  render(box: HTMLElement, call: MenuCall, rerender: () => void): void {
    const groups = menuGroups(call.art, call.plays);
    const group = groups.find((g) => g.formation.id === this.formation);
    if (!group) this.formation = null;
    const sug = call.suggestion?.play;
    const sugFormation = call.art.plays.find((p) => p.id === sug)?.formation;

    const tools = h("div", "prompt-tools");
    const flip = h("input");
    flip.type = "checkbox";
    flip.checked = this.flip;
    flip.addEventListener("change", () => {
      this.flip = flip.checked;
      flip.blur();
      rerender();
    });
    const flipLabel = h("label", "menu-flip");
    flipLabel.append(flip, " Flip");
    tools.append(flipLabel);
    for (const k of call.kicks) tools.append(this.button(CHOICE_NAMES[k] ?? k, { play: k }, k === sug ? "suggested" : ""));
    for (const c of call.clock) tools.append(this.button(c === "kneel" ? "Kneel" : "Spike", { play: c }));
    if (sug) {
      const like = h("span", "muted", `Coach likes: ${CHOICE_NAMES[sug] ?? call.art.plays.find((p) => p.id === sug)?.name ?? sug}${call.suggestion?.flip ? " (flipped)" : ""}`);
      tools.append(like);
    }
    if (!call.plays) tools.append(h("span", "muted", "The call isn't open yet: your pick goes in when it is."));

    const grid = h("div", "menu-grid");
    if (group) {
      const head = h("div", "menu-head");
      const back = h("button", "", "‹ Formations (Esc)");
      back.addEventListener("click", () => {
        this.back();
        rerender();
      });
      head.append(back, h("strong", "", group.formation.name), h("span", "muted", ` · ${group.formation.personnel} personnel`));
      box.append(tools, head, grid);
      for (const p of group.plays) {
        const flipped = this.flip && p.flip;
        const card = this.card(playSvg(p, group.formation, { flip: flipped }), p.name, { play: p.id, flip: flipped });
        if (p.id === sug && !!call.suggestion?.flip === flipped) card.classList.add("suggested");
        if (this.flip && !p.flip) card.append(h("span", "menu-note", "can't flip"));
        grid.append(card);
      }
      return;
    }
    box.append(tools, grid);
    for (const g of groups) {
      const card = h("button", "menu-card");
      card.innerHTML = formationSvg(g.formation, { flip: this.flip });
      card.append(h("div", "menu-name", g.formation.name), h("div", "menu-sub", `${g.formation.personnel} · ${g.plays.length} plays`));
      if (g.formation.id === sugFormation) card.classList.add("suggested");
      card.addEventListener("click", () => {
        this.formation = g.formation.id;
        rerender();
      });
      grid.append(card);
    }
    const missing = missingArt(call.art, call.plays);
    if (missing.length) {
      const other = h("div", "menu-other");
      other.append(h("span", "muted", "No picture: "));
      for (const p of missing) other.append(this.button(p.name || p.id, { play: p.id, flip: this.flip && p.flip }, p.id === sug ? "suggested" : ""));
      box.append(other);
    }
  }

  private button(text: string, answer: MenuAnswer, cls: string = ""): HTMLButtonElement {
    const b = h("button", cls, text);
    b.addEventListener("click", () => this.onPick(answer));
    return b;
  }

  private card(svg: string, name: string, answer: MenuAnswer): HTMLButtonElement {
    const b = h("button", "menu-card");
    b.innerHTML = svg;
    b.append(h("div", "menu-name", name));
    b.addEventListener("click", () => this.onPick(answer));
    return b;
  }
}
