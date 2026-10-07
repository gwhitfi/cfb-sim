// The play menu: offensive and defensive calls as pictures
// (docs/PROPOSALS.md#the-play-menu). Formations first (players only), then a
// formation's plays (routes, the first read's in red), each with its name.
// The pictures come from the server's playbook art (art.ts).

import {
  formationSvg, playSvg, defensiveFormationSvg, defensivePlaySvg,
  type FormationArt, type PlayArt, type PlaybookArt,
  type DefensiveFormationArt, type DefensivePlayArt, type DefensivePlaybookArt,
} from "./art";
import { CHOICE_NAMES, type PlayOption } from "./game";

// One formation and the plays the menu shows in it, in the playbook's order.
export type MenuGroup =
  | { unit: "offense"; formation: FormationArt; plays: PlayArt[] }
  | { unit: "defense"; formation: DefensiveFormationArt; plays: DefensivePlayArt[] };

type MenuArt = PlaybookArt | DefensivePlaybookArt;

// Keep each unit's formation/play types together while sharing the filtering.
function groupsFor<F extends { id: string }, P extends { id: string; formation: string }>(
  art: { formations: F[]; plays: P[] }, offered: PlayOption[] | null,
): { formation: F; plays: P[] }[] {
  const ids = offered ? new Set(offered.map((p) => p.id)) : null;
  return art.formations.map((formation) => ({
    formation, plays: art.plays.filter((p) => p.formation === formation.id && (!ids || ids.has(p.id))),
  })).filter((g) => g.plays.length > 0);
}

// Only offered plays, in book order; null means the early offensive menu.
export function menuGroups(art: MenuArt, offered: PlayOption[] | null): MenuGroup[] {
  return art.unit === "defense"
    ? groupsFor(art, offered).map((g) => ({ unit: "defense", ...g }))
    : groupsFor(art, offered).map((g) => ({ unit: "offense", ...g }));
}

// The call's plays that have no picture (the menu lists them by name so
// none is lost).
export function missingArt(art: MenuArt, offered: PlayOption[] | null): PlayOption[] {
  if (!offered) return [];
  const formations = new Set(art.formations.map((f) => f.id));
  const ids = new Set(art.plays.filter((p) => formations.has(p.formation)).map((p) => p.id));
  return offered.filter((p) => !ids.has(p.id));
}

// What the menu needs for one call.
export interface MenuCall {
  art: MenuArt;
  plays: PlayOption[] | null; // the prompt's plays; null before the call is open
  suggestion?: { play?: string; flip?: boolean } | null;
  kicks: string[]; // punt and field goal on 4th down, the extra point on a try
  clock: string[]; // kneel, spike
  stay?: { play: string; name: string }; // defensive check: the current call
}

// The answer a pick makes: {play, flip}, a kick or clock play {play}, or the coach's call {auto}.
export type MenuAnswer = Record<string, unknown>;

const h = <K extends keyof HTMLElementTagNameMap>(tag: K, cls = "", text = ""): HTMLElementTagNameMap[K] => {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text) e.textContent = text;
  return e;
};

// Width is the available picker width, not the whole browser window.
export const menuPageSize = (width: number): number => width >= 840 ? 3 : width >= 560 ? 2 : 1;

// IDs keep the anchor and focus stable when legal choices are refreshed.
export class MenuPage {
  ids: string[] = [];
  size = 3;
  page = 0;
  focused: string | null = null;
  get count(): number { return Math.ceil(this.ids.length / this.size); }
  get visible(): string[] { return this.ids.slice(this.page * this.size, (this.page + 1) * this.size); }

  sync(ids: string[], size: number): void {
    const anchor = this.visible[0];
    const index = anchor ? ids.indexOf(anchor) : -1;
    this.ids = ids;
    this.size = size;
    this.page = Math.max(0, Math.min(index >= 0 ? Math.floor(index / size) : this.page, this.count - 1));
    if (!this.visible.includes(this.focused ?? "")) this.focused = this.visible[0] ?? null;
  }
  cycle(direction: number): void {
    if (!this.count) return;
    this.page = (this.page + direction + this.count) % this.count;
    this.focused = this.visible[0] ?? null;
  }
  reveal(id: string): void {
    const index = this.ids.indexOf(id);
    if (index < 0) return;
    this.page = Math.floor(index / this.size);
    this.focused = id;
  }
  move(direction: number): void {
    if (!this.ids.length) return;
    const index = Math.max(0, this.ids.indexOf(this.focused ?? ""));
    this.reveal(this.ids[(index + direction + this.ids.length) % this.ids.length]!);
  }
}

export class PlayMenu {
  formation: string | null = null;
  flip = false;
  onPick: (answer: MenuAnswer) => void = () => {};
  private formations = new MenuPage();
  private plays = new MenuPage();
  private observer: ResizeObserver | null = null;
  private box: HTMLElement | null = null;
  private rerender: () => void = () => {};
  private focusTarget: string | null = null;
  private get screen(): MenuPage { return this.formation === null ? this.formations : this.plays; }

  disconnect(): void { this.observer?.disconnect(); }

  // Live calls this before replacing the old prompt DOM.
  rememberFocus(): void {
    const active = document.activeElement as HTMLElement | null;
    if (!this.focusTarget && active && this.box?.closest(".prompt")?.contains(active)) {
      this.focusTarget = active.dataset.menuFocus ?? (active.matches(".prompt-head .coach") ? "coach" : null);
    }
  }

  back(): boolean {
    if (this.formation === null) return false;
    this.formation = null;
    this.focusTarget = "card";
    return true;
  }

  key(e: KeyboardEvent): boolean {
    const target = e.target instanceof HTMLElement ? e.target : document.activeElement as HTMLElement | null;
    if (e.altKey || e.ctrlKey || e.metaKey || e.shiftKey || target?.closest("input, textarea, select, [contenteditable]:not([contenteditable=false])")) return false;
    if (e.key === "Escape") {
      if (!this.back()) return false;
      this.rerender();
      return true;
    }
    if (e.key === "ArrowLeft" || e.key === "ArrowRight") {
      if (!this.screen.ids.length) return true;
      this.screen.move(e.key === "ArrowLeft" ? -1 : 1);
      this.focusTarget = "card";
      this.rerender();
      return true;
    }
    if (e.key === "Enter" || e.key === " ") {
      // Handle activation here so Space does not toggle playback in main.ts.
      const button = target?.closest("button");
      if (button && this.box?.closest(".prompt")?.contains(button)) {
        if (!e.repeat) button.click();
        return true;
      }
    }
    return false;
  }

  render(box: HTMLElement, call: MenuCall, rerender: () => void): void {
    this.disconnect();
    this.box = box;
    this.rerender = rerender;
    const size = menuPageSize(box.clientWidth);
    const groups = menuGroups(call.art, call.plays);
    const group = groups.find((g) => g.formation.id === this.formation);
    if (!group) this.formation = null;
    const missing = missingArt(call.art, call.plays);
    const sug = call.suggestion?.play;
    const sugGroup = groups.find((g) => g.plays.some((p) => p.id === sug));
    const defense = call.art.unit === "defense";
    const playKey = (id: string) => `play:${id}`;
    const formationKey = (id: string) => `formation:${id}`;
    const tools = h("div", "prompt-tools");
    const action = (name: string, answer: MenuAnswer, id: string, cls = "") => {
      const b = this.button(name, answer, cls);
      b.dataset.menuFocus = `action:${id}`;
      tools.append(b);
      return b;
    };
    if (call.stay) action(`Stay with ${call.stay.name}`, { play: call.stay.play }, call.stay.play, call.stay.play === sug ? "coach suggested" : "coach");
    if (!defense) {
      const flip = h("input");
      flip.type = "checkbox";
      flip.checked = this.flip;
      flip.dataset.menuFocus = "flip";
      flip.addEventListener("change", () => { this.flip = flip.checked; rerender(); });
      const label = h("label", "menu-flip");
      label.append(flip, " Flip");
      tools.append(label);
      for (const k of call.kicks) action(CHOICE_NAMES[k] ?? k, { play: k }, k, k === sug ? "suggested" : "");
      for (const c of call.clock) action(c === "kneel" ? "Kneel" : "Spike", { play: c }, c, c === sug ? "suggested" : "");
    }
    if (sug) {
      const suggestion = h("div", "menu-suggestion");
      tools.append(suggestion);
      suggestion.append(h("span", "muted", `Coach likes: ${CHOICE_NAMES[sug] ?? call.art.plays.find((p) => p.id === sug)?.name ?? call.plays?.find((p) => p.id === sug)?.name ?? sug}${!defense && call.suggestion?.flip ? " (flipped)" : ""}`));
      if (sugGroup || missing.some((p) => p.id === sug) || call.stay?.play === sug || (!defense && [...call.kicks, ...call.clock].includes(sug))) {
        const show = h("button", "", "Show suggestion");
        show.dataset.menuFocus = "suggestion";
        show.addEventListener("click", () => {
          if (call.stay?.play === sug || (!defense && [...call.kicks, ...call.clock].includes(sug))) {
            this.focusTarget = `action:${sug}`;
          } else {
            if (sugGroup && this.formation !== sugGroup.formation.id) {
              this.formation = sugGroup.formation.id;
              this.plays = new MenuPage();
            }
            if (!defense) this.flip = !!call.suggestion?.flip;
            // Build the destination before looking up a page in it.
            rerender();
            this.screen.reveal(playKey(sug));
            this.focusTarget = "card";
          }
          rerender();
        });
        suggestion.append(show);
      }
    }
    if (!call.plays) tools.append(h("span", "muted menu-early-note", "The call isn't open yet: your pick goes in when it is."));
    box.append(tools);

    // Cards have stable IDs; only the visible page is attached to the DOM.
    const cards = new Map<string, HTMLButtonElement>();
    if (group) {
      const head = h("div", "menu-head");
      const back = h("button", "", "‹ Formations (Esc)");
      back.dataset.menuFocus = "back";
      back.addEventListener("click", () => { this.back(); rerender(); });
      head.append(back, h("strong", "", group.formation.name), h("span", "muted", group.unit === "defense" ? ` · ${group.formation.package} package` : ` · ${group.formation.personnel} personnel`));
      box.append(head);
      if (group.unit === "defense") {
        for (const p of group.plays) {
          const card = this.card(defensivePlaySvg(p), p.name, { play: p.id });
          card.append(h("div", "menu-sub", p.coverage.replaceAll("_", " ").replace(/\b\w/g, (c) => c.toUpperCase())));
          if (p.id === sug) card.classList.add("suggested");
          cards.set(playKey(p.id), card);
        }
      } else {
        for (const p of group.plays) {
          const flipped = this.flip && p.flip;
          const card = this.card(playSvg(p, group.formation, { flip: flipped }), p.name, { play: p.id, flip: flipped });
          if (p.id === sug && !!call.suggestion?.flip === flipped) card.classList.add("suggested");
          if (this.flip && !p.flip) card.append(h("span", "menu-note", "can't flip"));
          cards.set(playKey(p.id), card);
        }
      }
    } else {
      box.append(h("div", "menu-section", groups.length ? "Select formation" : "Select play"));
      for (const g of groups) {
        const card = h("button", "menu-card");
        card.innerHTML = g.unit === "defense" ? defensiveFormationSvg(g.formation) : formationSvg(g.formation, { flip: this.flip });
        card.append(h("div", "menu-name", g.formation.name), h("div", "menu-sub", `${g.unit === "defense" ? g.formation.package : g.formation.personnel} · ${g.plays.length} plays`));
        if (g === sugGroup) card.classList.add("suggested");
        card.addEventListener("click", () => {
          this.formations.focused = formationKey(g.formation.id);
          this.formation = g.formation.id;
          this.plays = new MenuPage();
          this.focusTarget = "card";
          rerender();
        });
        cards.set(formationKey(g.formation.id), card);
      }
    }
    for (const p of missing) {
      const card = this.card("", p.name || p.id, defense ? { play: p.id } : { play: p.id, flip: this.flip && p.flip });
      card.prepend(h("div", "menu-placeholder", "Diagram unavailable"));
      if (p.id === sug && (defense || !!call.suggestion?.flip === (this.flip && p.flip))) card.classList.add("suggested");
      cards.set(playKey(p.id), card);
    }
    const screen = this.screen;
    screen.sync([...cards.keys()], size);
    const nav = h("div", "menu-pages");
    const previous = h("button", "", "Previous");
    const next = h("button", "", "Next");
    for (const [b, direction, id] of [[previous, -1, "previous"], [next, 1, "next"]] as const) {
      b.disabled = screen.count <= 1;
      b.setAttribute("aria-label", `${id === "next" ? "Next" : "Previous"} page`);
      b.dataset.menuFocus = id;
      b.addEventListener("click", () => { screen.cycle(direction); this.focusTarget = id; rerender(); });
    }
    const count = h("span", "", `Page ${screen.count ? screen.page + 1 : 0} of ${screen.count}`);
    count.setAttribute("aria-live", "polite");
    nav.append(previous, count, next);
    const grid = h("div", "menu-grid");
    grid.style.gridTemplateColumns = `repeat(${size}, minmax(0, 1fr))`;
    for (const id of screen.visible) {
      const card = cards.get(id)!;
      card.dataset.menuFocus = "card";
      card.dataset.cardId = id;
      card.tabIndex = screen.focused === id ? 0 : -1;
      card.addEventListener("focus", () => {
        screen.focused = id;
        for (const c of grid.querySelectorAll<HTMLButtonElement>(".menu-card")) c.tabIndex = c === card ? 0 : -1;
      });
      grid.append(card);
    }
    if (!cards.size) grid.append(h("div", "muted", call.stay ? "No alternative calls available." : "No plays available."));
    box.append(grid, nav);
    if (this.focusTarget) {
      let focus = this.focusTarget === "coach" ? box.closest(".prompt")?.querySelector<HTMLElement>(".prompt-head .coach") ?? undefined :
        this.focusTarget === "card" ? cards.get(screen.focused ?? "") :
        [...box.querySelectorAll<HTMLElement>("[data-menu-focus]")].find((e) => e.dataset.menuFocus === this.focusTarget);
      if (focus instanceof HTMLButtonElement && focus.disabled) focus = cards.get(screen.focused ?? "");
      focus?.focus({ preventScroll: true });
      // Short panels scroll internally: keep the keyboard destination visible.
      if (focus) {
        const panel = box.closest(".prompt")!.getBoundingClientRect();
        const rect = focus.getBoundingClientRect();
        if (rect.top < panel.top || rect.bottom > panel.bottom) focus.scrollIntoView({ block: "nearest", inline: "nearest" });
      }
      this.focusTarget = null;
    }
    this.observer = new ResizeObserver(() => {
      if (box.isConnected && box.getClientRects().length && menuPageSize(box.clientWidth) !== screen.size) {
        this.rememberFocus();
        rerender();
      }
    });
    this.observer.observe(box);
  }

  private button(text: string, answer: MenuAnswer, cls = ""): HTMLButtonElement {
    const b = h("button", cls, text);
    b.addEventListener("click", () => this.onPick(answer));
    return b;
  }
  private card(svg: string, name: string, answer: MenuAnswer): HTMLButtonElement {
    const b = this.button("", answer, "menu-card");
    b.innerHTML = svg;
    b.append(h("div", "menu-name", name));
    return b;
  }
}
