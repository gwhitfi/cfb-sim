// Live games and full-game playback in the page (phase 11): the connection
// to cfbsim-server, the scoreboard, the play-by-play feed, and the prompts
// (play calls, the hike with audibles and hot routes, timeouts, 4th down,
// penalties). Each snap's tracking file plays in the normal viewer.

import type { Playback } from "./playback";
import {
  byFormation,
  CHOICE_NAMES,
  describeLine,
  downText,
  isScore,
  penaltyName,
  periodText,
  promptTitle,
  readyReading,
  StepQueue,
  type AdvanceResult,
  type GameState,
  type PlayOption,
  type Prompt,
  type TeamKey,
} from "./game";

// What the live mode needs from the page.
export interface Host {
  // Loads a snap's tracking file and starts playing it. False if it can't be read.
  show(text: string, name: string): boolean;
  playback(): Playback | null;
}

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;

const HOLD_AFTER_SNAP = 0.9; // s to linger on a play's last frame
const SERVER_KEY = "cfbsim.server";

// A new element with properties (textContent, className, ...) and children.
function el<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  props: Record<string, unknown> = {},
  ...children: (Node | string)[]
): HTMLElementTagNameMap[K] {
  const e: HTMLElementTagNameMap[K] = document.createElement(tag);
  Object.assign(e, props);
  e.append(...children);
  return e;
}

function button(text: string, onClick: () => void, cls = ""): HTMLButtonElement {
  const b = el("button", { textContent: text, className: cls });
  b.addEventListener("click", onClick);
  return b;
}

export class Live {
  private ws: WebSocket | null = null;
  private queue = new StepQueue();
  private prompt: Prompt | null = null;
  private answering: Prompt | null = null; // the prompt an answer is in flight for
  private errorText = "";
  private shownPrompt: Prompt | null = null;
  private done = false;
  private waiting = false; // an advance or command is in flight
  private animating = false;
  private hold = 0;
  private state: GameState | null = null;
  private mode: "live" | "replay" | null = null;
  private user = new Set<TeamKey>();
  private auto = new Set<TeamKey>(); // user sides handed to the coach
  private clockReading = 0; // the hike prompt's play clock
  private clockFrozen = false;
  private hikeSent = false;
  private tempo = "huddle";
  private names = new Map<string, string>(); // player id -> name, from the tracking files

  constructor(private host: Host) {
    $("game-toggle").addEventListener("click", () => this.togglePanel());
    $<HTMLInputElement>("game-server").value = localStorage.getItem(SERVER_KEY) ?? "ws://localhost:8765";
    $("game-connect").addEventListener("click", () => this.connect());
    $("game-start").addEventListener("click", () => this.start());
    $("game-watch").addEventListener("click", () => this.watch());
    $("game-stop").addEventListener("click", () => this.stop());
    $("game-timeout").addEventListener("click", () => this.timeout());
    $<HTMLInputElement>("game-auto").addEventListener("change", (e) => this.setAuto((e.target as HTMLInputElement).checked));
  }

  get active(): boolean {
    return this.mode !== null;
  }

  // ---------------------------------------------------------------------
  // Connection
  // ---------------------------------------------------------------------

  private togglePanel(): void {
    const panel = $("game");
    panel.hidden = !panel.hidden;
    if (!panel.hidden && !this.ws) this.connect();
  }

  private connect(): void {
    const url = $<HTMLInputElement>("game-server").value.trim();
    localStorage.setItem(SERVER_KEY, url);
    this.ws?.close();
    this.status(`Connecting to ${url}…`);
    const ws = new WebSocket(url);
    this.ws = ws;
    ws.onopen = () => this.send({ type: "hello" });
    ws.onerror = () => this.status(`Can't reach ${url}. Start the server: ./build/release/server/cfbsim-server`, true);
    ws.onclose = () => {
      if (this.ws === ws) {
        this.ws = null;
        this.status("Disconnected", true);
      }
    };
    ws.onmessage = (e) => this.receive(JSON.parse(e.data as string));
  }

  private send(msg: unknown): void {
    if (this.ws?.readyState === WebSocket.OPEN) this.ws.send(JSON.stringify(msg));
  }

  private status(text: string, bad = false): void {
    const s = $("game-status");
    s.textContent = text;
    s.classList.toggle("bad", bad);
  }

  private receive(m: any): void {
    switch (m.type) {
      case "hello": {
        this.status(`Connected · sim v${m.version}`);
        const teams = m.teams as { id: string; name: string }[];
        for (const [id, def] of [["game-home", 0], ["game-away", 1]] as const) {
          const sel = $<HTMLSelectElement>(id);
          sel.replaceChildren(...teams.map((t) => new Option(t.name, t.id)));
          sel.selectedIndex = Math.min(def, teams.length - 1);
        }
        $<HTMLSelectElement>("game-logs").replaceChildren(
          new Option(m.logs.length ? "Pick a game log…" : "No logs yet", ""),
          ...(m.logs as string[]).map((f) => new Option(f, f)),
        );
        break;
      }
      case "started":
        this.begin(m.mode);
        break;
      case "advance":
        this.waiting = false;
        this.onAdvance(m.result as AdvanceResult);
        break;
      case "command":
        this.waiting = false;
        if (!m.ok) {
          this.promptError(m.error);
          this.hikeSent = false;
          return;
        }
        this.advance();
        break;
      case "saved":
        this.feed(`Game log saved: ${m.path}`, "note");
        break;
      case "error":
        this.waiting = false;
        this.status(m.message, true);
        break;
    }
  }

  // ---------------------------------------------------------------------
  // Starting and stopping
  // ---------------------------------------------------------------------

  private start(): void {
    const side = $<HTMLSelectElement>("game-side").value;
    const user: TeamKey[] = side === "both" ? ["home", "away"] : side === "none" ? [] : [side as TeamKey];
    this.user = new Set(user);
    const seedText = $<HTMLInputElement>("game-seed").value.trim();
    const msg: any = {
      type: "new_game",
      home: $<HTMLSelectElement>("game-home").value,
      away: $<HTMLSelectElement>("game-away").value,
      user,
    };
    if (seedText) msg.seed = Number(seedText);
    this.send(msg);
  }

  private watch(): void {
    const file = $<HTMLSelectElement>("game-logs").value;
    if (!file) return;
    this.user.clear();
    this.send({ type: "replay", file });
  }

  private begin(mode: "live" | "replay"): void {
    this.mode = mode;
    this.queue.clear();
    this.prompt = this.shownPrompt = null;
    this.done = false;
    this.animating = false;
    this.auto.clear();
    $("feed").replaceChildren();
    $("game-setup").hidden = true;
    $("game-live").hidden = false;
    $("game-mine").hidden = this.user.size === 0;
    $<HTMLInputElement>("game-auto").checked = false;
    this.hidePrompt();
    this.advance();
  }

  private stop(): void {
    this.mode = null;
    this.queue.clear();
    this.hidePrompt();
    $("scoreboard").hidden = true;
    $("game-setup").hidden = false;
    $("game-live").hidden = true;
    this.send({ type: "hello" }); // refresh the logs list
  }

  private advance(): void {
    if (this.waiting || !this.mode) return;
    this.waiting = true;
    this.send({ type: "advance" });
  }

  private onAdvance(r: AdvanceResult): void {
    this.queue.push(r);
    this.prompt = r.prompt;
    this.done = r.done;
    if (r.error) this.feed(`Error: ${r.error}`, "bad");
  }

  // ---------------------------------------------------------------------
  // The per-frame loop: show queued snaps and lines, then the prompt
  // ---------------------------------------------------------------------

  tick(dt: number): void {
    if (!this.mode) return;
    if (this.shownPrompt?.input === "hike") this.tickHike(dt);
    if (this.animating) {
      const pb = this.host.playback();
      if (pb && (pb.playing || !pb.atEnd)) return;
      this.hold -= dt * (pb?.speed ?? 1);
      if (this.hold > 0) return;
      this.animating = false;
    }
    while (!this.queue.empty) {
      const step = this.queue.shift()!;
      if (step.kind === "line") {
        this.applyLine(step.line, step.state);
        continue;
      }
      const skip = $<HTMLInputElement>("game-skip").checked;
      if (this.host.show(step.data, `${step.name}.jsonl`)) {
        const pb = this.host.playback()!;
        for (const p of pb.rec.header.players) this.names.set(p.id, p.name);
        if (skip) {
          pb.playing = false;
          pb.seek(pb.lastFrame);
          this.hold = 0.15;
        } else {
          this.hold = HOLD_AFTER_SNAP;
        }
        this.animating = true;
        return;
      }
    }
    if (this.prompt) {
      if (this.shownPrompt !== this.prompt) this.showPrompt(this.prompt);
      return;
    }
    if (this.done) {
      $("game-mine").hidden = true;
      return;
    }
    this.advance();
  }

  private applyLine(line: any, state: GameState): void {
    this.state = state;
    this.scoreboard(state);
    const text = describeLine(line, state, this.names);
    if (text) this.feed(text, line.type === "play" ? (isScore(line) ? "score" : "play") : "event");
  }

  private feed(text: string, cls: string): void {
    const list = $("feed");
    list.prepend(el("li", { className: cls, textContent: text }));
  }

  private scoreboard(s: GameState): void {
    const box = $("scoreboard");
    box.hidden = false;
    const team = (k: TeamKey) => {
      const dots = "●".repeat(Math.max(0, s.timeouts[k])) + "○".repeat(Math.max(0, 3 - s.timeouts[k]));
      return el(
        "div",
        { className: `sb-team${s.offense === k ? " ball" : ""}${this.user.has(k) ? " mine" : ""}` },
        el("span", { className: "sb-abbr", textContent: s.teams[k].abbr }),
        el("span", { className: "sb-score", textContent: String(s.score[k]) }),
        el("span", { className: "sb-to", textContent: dots, title: `${s.timeouts[k]} timeouts left` }),
      );
    };
    box.replaceChildren(
      team("away"),
      team("home"),
      el("div", { className: "sb-clock" }, el("span", { textContent: periodText(s) }), el("strong", { textContent: s.overtime ? "" : s.clock_text })),
      el("div", { className: "sb-down", textContent: `${downText(s)} · ${s.situation.replace(/^.* at /, "")}` }),
      el("div", { className: "sb-play-clock", id: "sb-play-clock" }),
    );
  }

  // ---------------------------------------------------------------------
  // Prompts
  // ---------------------------------------------------------------------

  private answer(fields: Record<string, unknown>): void {
    if (this.waiting) return;
    this.waiting = true;
    this.send({ type: "command", command: { type: "answer", ...fields } });
    this.answering = this.prompt;
    this.prompt = null;
    this.hidePrompt();
  }

  private hidePrompt(): void {
    this.shownPrompt = null;
    $("prompt").hidden = true;
    const pc = document.getElementById("sb-play-clock");
    if (pc) pc.textContent = "";
  }

  private promptError(message: string): void {
    // The prompt is still pending on the server: show it again with the error.
    this.errorText = message;
    if (!this.prompt && this.answering) this.prompt = this.answering;
    this.shownPrompt = null;
  }

  private showPrompt(p: Prompt): void {
    this.shownPrompt = p;
    this.scoreboard(p.state);
    const box = $("prompt");
    box.hidden = false;
    const head = el(
      "div",
      { className: "prompt-head" },
      el("strong", { textContent: `${p.state.teams[p.team].abbr}: ${promptTitle(p)}` }),
      el("span", { className: "muted", textContent: ` ${p.state.situation} · ${periodText(p.state)} ${p.state.clock_text}` }),
      button("Coach's call", () => this.answer({ auto: true }), "coach"),
    );
    const body = el("div", { className: "prompt-body" });
    box.replaceChildren(head, body);
    if (this.errorText) {
      box.insertBefore(el("div", { className: "prompt-error", textContent: this.errorText }), body);
      this.errorText = "";
    }
    switch (p.input) {
      case "call_play":
        this.callPlay(p, body);
        break;
      case "hike":
        this.hike(p, body);
        break;
      case "coaching_decision":
        for (const c of p.options.choices as string[]) {
          const b = button(CHOICE_NAMES[c] ?? c, () => this.answer({ choice: c }), c === p.suggestion?.choice ? "suggested" : "");
          body.append(b);
        }
        break;
      case "penalty_decision": {
        const o = p.options;
        body.append(el("div", { className: "muted", textContent: o.play }));
        body.append(button(`Decline: ${o.declined}`, () => this.answer({ accept: false }), p.suggestion?.accept ? "" : "suggested"));
        for (const f of o.fouls) {
          const label = `Accept ${penaltyName(f.foul)} (${f.yards > 0 ? "+" : ""}${f.yards} yd): ${f.result}`;
          const suggested = p.suggestion?.accept && p.suggestion.foul === f.foul;
          body.append(button(label, () => this.answer({ accept: true, foul: f.foul }), suggested ? "suggested" : ""));
        }
        break;
      }
      case "runoff":
        body.append(
          button(`Take ${p.options.seconds} s off`, () => this.answer({ take: true }), p.suggestion?.take ? "suggested" : ""),
          button("No runoff", () => this.answer({ take: false }), p.suggestion?.take ? "" : "suggested"),
        );
        break;
      default:
        body.append(el("div", { textContent: JSON.stringify(p.options) }));
    }
  }

  private callPlay(p: Prompt, body: HTMLElement): void {
    const o = p.options;
    const flip = el("input", { type: "checkbox" });
    const tools = el("div", { className: "prompt-tools" });
    if (o.side === "offense") tools.append(el("label", {}, flip, " Flip"));
    if (o.clock_plays) {
      for (const c of o.clock_plays as string[]) tools.append(button(c === "kneel" ? "Kneel" : "Spike", () => this.answer({ play: c })));
    }
    const sug = p.suggestion?.play;
    tools.append(el("span", { className: "muted", textContent: sug ? `Coach likes: ${sug}${p.suggestion.flip ? " (flipped)" : ""}` : "" }));
    body.append(tools);
    const grid = el("div", { className: "play-grid" });
    for (const [formation, plays] of byFormation(o.plays as PlayOption[])) {
      const col = el("div", { className: "play-group" }, el("div", { className: "group-name", textContent: formation }));
      for (const play of plays) {
        const b = button(play.name || play.id, () => {
          const f = flip.checked && play.flip;
          this.answer(o.side === "offense" ? { play: play.id, flip: f } : { play: play.id });
        }, play.id === sug ? "suggested" : "");
        b.title = `${play.id} · ${play.type}${play.tags.length ? " · " + play.tags.join(", ") : ""}${play.flip ? "" : " · can't flip"}`;
        col.append(b);
      }
      grid.append(col);
    }
    body.append(grid);
  }

  private hike(p: Prompt, body: HTMLElement): void {
    const o = p.options;
    this.hikeSent = false;
    this.clockReading = readyReading(o, this.tempo);
    const tempo = el("select", {}, ...["huddle", "no_huddle", "hurry"].map((t) => new Option(t.replace("_", "-"), t)));
    tempo.value = this.tempo;
    tempo.addEventListener("change", () => {
      this.tempo = tempo.value;
      this.clockReading = Math.min(this.clockReading, readyReading(o, this.tempo));
      if (tempo.value !== "huddle") this.clockReading = readyReading(o, this.tempo);
    });
    const freeze = el("input", { type: "checkbox", checked: this.clockFrozen });
    freeze.addEventListener("change", () => (this.clockFrozen = freeze.checked));
    const hikeBtn = button("Hike (H)", () => this.sendHike(), "hike");
    hikeBtn.id = "hike-button";
    body.append(
      el("div", { className: "prompt-tools" },
        hikeBtn,
        el("label", {}, "Tempo ", tempo),
        el("label", {}, freeze, " Freeze play clock"),
        el("span", { className: "muted", textContent: o.play ? `Called: ${o.play}` : "Kick" }),
      ),
    );

    // Audible and hot routes.
    const audible = el("select", {}, new Option("No audible", ""), ...(o.audibles as string[]).map((a) => new Option(a, a)));
    const hot = new Map<string, string>();
    const hotList = el("span", { className: "muted" });
    const slot = el("select", {}, ...(o.hot_slots as string[]).map((s) => new Option(s, s)));
    const route = el("select", {}, ...(o.routes as string[]).map((r) => new Option(r, r)));
    const showHot = () => (hotList.textContent = [...hot].map(([s, r]) => `${s}: ${r}`).join(", "));
    const addHot = button("Set route", () => {
      hot.set(slot.value, route.value);
      audible.value = "";
      showHot();
    });
    const clearHot = button("Clear", () => {
      hot.clear();
      showHot();
    });
    audible.addEventListener("change", () => {
      if (audible.value) {
        hot.clear();
        showHot();
      }
    });
    if (o.audibles.length) body.append(el("div", { className: "prompt-tools" }, el("label", {}, "Audible ", audible)));
    if (o.hot_slots.length) {
      body.append(el("div", { className: "prompt-tools" }, el("label", {}, "Hot route ", slot, " ", route), addHot, clearHot, hotList));
    }
    this.hikeFields = () => {
      const f: Record<string, unknown> = {};
      if (audible.value) f.audible = audible.value;
      if (hot.size) f.hot_routes = Object.fromEntries(hot);
      return f;
    };
    this.updatePlayClock();
  }

  private hikeFields: () => Record<string, unknown> = () => ({});

  private sendHike(delay = false): void {
    if (this.hikeSent || this.shownPrompt?.input !== "hike") return;
    this.hikeSent = true;
    const reading = delay ? -0.1 : Math.round(this.clockReading * 100) / 100;
    this.answer({ play_clock: reading, ...this.hikeFields() });
  }

  private tickHike(dt: number): void {
    if (this.hikeSent) return;
    if (!this.clockFrozen) this.clockReading -= dt;
    this.updatePlayClock();
    if (this.clockReading <= 0) this.sendHike(true); // delay of game
  }

  private updatePlayClock(): void {
    const pc = document.getElementById("sb-play-clock");
    if (pc) pc.textContent = `:${Math.max(0, Math.ceil(this.clockReading)).toString().padStart(2, "0")}`;
    pc?.classList.toggle("low", this.clockReading < 5);
  }

  // ---------------------------------------------------------------------
  // Commands any time: timeouts and the auto toggle
  // ---------------------------------------------------------------------

  // The side the buttons act for: the one being prompted, or the only user side.
  private mySide(): TeamKey | null {
    if (this.shownPrompt && this.user.has(this.shownPrompt.team)) return this.shownPrompt.team;
    if (this.user.size === 1) return [...this.user][0]!;
    return this.state && this.user.has(this.state.offense) ? this.state.offense : null;
  }

  private timeout(): void {
    const team = this.mySide();
    if (!team) return;
    const cmd: Record<string, unknown> = { type: "timeout", team };
    if (this.shownPrompt?.input === "hike") {
      cmd.play_clock = Math.max(0, Math.round(this.clockReading * 100) / 100);
      this.hikeSent = true;
      this.prompt = null;
      this.hidePrompt();
    }
    this.waiting = true;
    this.send({ type: "command", command: cmd });
    this.feed(`${team === "home" ? "Home" : "Away"} calls timeout`, "note");
  }

  private setAuto(on: boolean): void {
    for (const team of this.user) {
      if (on) this.auto.add(team);
      else this.auto.delete(team);
      this.send({ type: "command", command: { type: "auto", team, on } });
    }
    if (on && this.prompt && this.user.has(this.prompt.team)) {
      this.prompt = null;
      this.hidePrompt();
    }
  }

  // Keyboard shortcuts while a prompt is up. Returns true if handled.
  key(e: KeyboardEvent): boolean {
    if (!this.shownPrompt) return false;
    if ((e.key === "h" || e.key === "Enter") && this.shownPrompt.input === "hike") {
      this.sendHike();
      return true;
    }
    return false;
  }
}
