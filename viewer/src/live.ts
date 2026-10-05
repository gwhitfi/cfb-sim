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
  playClockLeft,
  promptTitle,
  StepQueue,
  type AdvanceResult,
  type GameState,
  type PlayOption,
  type Prompt,
  type TeamKey,
} from "./game";
import { PlayRunner, type PlayReply, type TeamInfo } from "./plays";
import { StatsPanel } from "./stats";

// What the live mode needs from the page.
export interface Host {
  // Loads a snap's tracking file and starts playing it. False if it can't be read.
  show(text: string, name: string): boolean;
  playback(): Playback | null;
  devMode(): boolean; // plays only snap to whistle: no between-plays files
  // The teams and game logs from the server's hello.
  serverInfo(teams: TeamInfo[], logs: string[]): void;
}

// What New Game asks for.
export interface GameSetup {
  home: string;
  away: string;
  user: TeamKey[]; // the sides the user coaches (none: watch)
  seed?: number;
  auto: boolean; // the coach calls the user's plays
  skip: boolean; // skip animations
}

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;

const HOLD_AFTER_SNAP = 0.9; // s to linger on a play's last frame
const LOOKAHEAD = 8; // ticks: between plays, the game runs this far ahead of the screen

// A between-plays file streaming in while the play clock runs (game-flow.md).
interface LiveWindow {
  name: string;
  text: string;
  done: boolean; // the last part is in
  start: number; // the game tick its tick 0 is
  playClock: number;
  waits: boolean; // the play clock starts at the "ready" event
  shown: boolean; // loaded in the viewer
  dirty: boolean; // new parts since
  hiddenTick?: number; // skip animations or dev mode: the clock we keep instead (file ticks)
}

const key = (p: { input: string; team: string }) => `${p.input}/${p.team}`;
const SERVER_KEY = "cfbsim.server";
export const DEFAULT_SERVER = "ws://localhost:8765";

export function serverUrl(): string {
  try {
    return localStorage.getItem(SERVER_KEY) || DEFAULT_SERVER;
  } catch {
    return DEFAULT_SERVER;
  }
}

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
  private between = false; // the file playing is a between-plays one
  private mode: "live" | "replay" | null = null;
  private user = new Set<TeamKey>();
  private auto = new Set<TeamKey>(); // user sides handed to the coach
  private live: Prompt[] = []; // the user's open decisions between plays
  private answeredLive = new Set<string>(); // answered (still open on the server until it's applied)
  private lastLive = ""; // the decision an answer is in flight for
  private inWindow = false; // the game is stopped between plays, waiting for the clock
  private simTick = 0;
  private win: LiveWindow | null = null;
  private names = new Map<string, string>(); // player id -> name, from the tracking files
  private plays: PlayRunner;
  private stats = new StatsPanel($("game-stats"));
  private setup: GameSetup | null = null; // the game being started
  teams: TeamInfo[] = [];
  logs: string[] = [];
  statusText = "";
  statusBad = false;

  constructor(private host: Host) {
    $("game-stop").addEventListener("click", () => this.stop());
    $("game-timeout").addEventListener("click", () => this.timeout());
    $("game-hurry").addEventListener("click", () => this.hurry());
    const auto = $<HTMLInputElement>("game-auto");
    auto.addEventListener("change", () => {
      this.setAuto(auto.checked);
      auto.blur(); // so the keyboard shortcuts keep working
    });
    $("game-skip").addEventListener("change", (e) => (e.target as HTMLElement).blur());
    // Plays mode (plays.ts) shares this connection.
    this.plays = new PlayRunner({
      send: (msg) => this.send(msg),
      connect: () => this.ensureConnected(),
      show: (text, name) => this.host.show(text, name),
    });
  }

  get active(): boolean {
    return this.mode !== null;
  }

  // ---------------------------------------------------------------------
  // Connection
  // ---------------------------------------------------------------------

  get connected(): boolean {
    return this.ws?.readyState === WebSocket.OPEN;
  }

  // Connects unless already connected (or connecting); if connected, asks for
  // the teams and game logs again.
  ensureConnected(): void {
    if (!this.ws || this.ws.readyState > WebSocket.OPEN) this.connect();
    else this.send({ type: "hello" });
  }

  // Connects to `url` (and remembers it), or to the saved server.
  connect(url = serverUrl()): void {
    try {
      localStorage.setItem(SERVER_KEY, url);
    } catch {
      // Not remembered.
    }
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
    this.statusText = text;
    this.statusBad = bad;
    for (const s of document.querySelectorAll<HTMLElement>("[data-status]")) {
      s.textContent = text;
      s.classList.toggle("bad", bad);
    }
    this.plays.serverStatus(text, bad);
  }

  private receive(m: any): void {
    switch (m.type) {
      case "hello": {
        this.status(`Connected · sim v${m.version}`);
        this.teams = m.teams as TeamInfo[];
        this.logs = m.logs as string[];
        this.plays.setTeams(this.teams);
        this.host.serverInfo(this.teams, this.logs);
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
          this.answeredLive.delete(this.lastLive);
          this.promptError(m.error);
          return;
        }
        this.advance();
        break;
      case "play":
        this.plays.onResult(m as PlayReply);
        break;
      case "saved":
        this.feed(`Game log saved: ${m.path}`, "note");
        break;
      case "error":
        if (this.plays.onError(m.message)) break;
        this.waiting = false;
        this.status(m.message, true);
        break;
    }
  }

  // ---------------------------------------------------------------------
  // Starting and stopping
  // ---------------------------------------------------------------------

  // Kicks off a live game. False if there's no connection.
  start(g: GameSetup): boolean {
    if (!this.connected) return false;
    this.setup = g;
    this.user = new Set(g.user);
    const msg: Record<string, unknown> = { type: "new_game", home: g.home, away: g.away, user: g.user };
    if (g.seed !== undefined) msg.seed = g.seed;
    this.send(msg);
    return true;
  }

  // Watches a game log (one of `logs`). False if there's no connection.
  watch(file: string, skip: boolean): boolean {
    if (!this.connected) return false;
    this.setup = { home: "", away: "", user: [], auto: false, skip };
    this.user.clear();
    this.send({ type: "replay", file });
    return true;
  }

  private begin(mode: "live" | "replay"): void {
    this.mode = mode;
    this.queue.clear();
    this.prompt = this.shownPrompt = null;
    this.live = [];
    this.answeredLive.clear();
    this.win = null;
    this.inWindow = false;
    this.done = false;
    this.animating = false;
    this.auto.clear();
    $("feed").replaceChildren();
    this.stats.clear();
    $("game").hidden = false;
    this.showMine(this.user.size > 0);
    $("tb-skip").hidden = false;
    $<HTMLInputElement>("game-auto").checked = false;
    $<HTMLInputElement>("game-skip").checked = this.setup?.skip ?? false;
    this.hidePrompt();
    if (this.setup?.auto && this.user.size) {
      $<HTMLInputElement>("game-auto").checked = true;
      this.setAuto(true);
    }
    this.setup = null;
    // Coaching: the screen sets the pace between plays from the first one.
    if (mode === "live" && this.user.size > 0) this.send({ type: "command", command: { type: "run_to", tick: 0 } });
    this.advance();
  }

  private stop(): void {
    this.mode = null;
    this.queue.clear();
    this.win = null;
    this.live = [];
    this.hidePrompt();
    $("scoreboard").hidden = true;
    $("game").hidden = true;
    $("tb-skip").hidden = true;
    this.showMine(false);
    this.send({ type: "hello" }); // refresh the logs list
  }

  // The controls for the user's own side: Timeout, and Auto coach in the transport bar.
  private showMine(show: boolean): void {
    $("game-timeout").hidden = !show;
    $("game-hurry").hidden = !show;
    $("tb-auto").hidden = !show;
  }

  private advance(): void {
    if (this.waiting || !this.mode) return;
    this.waiting = true;
    this.send({ type: "advance" });
  }

  private onAdvance(r: AdvanceResult): void {
    // More of the between-plays file on screen goes straight to it.
    const rest = r.messages.filter((m) => {
      if (m.type !== "tracking" || !this.win || m.name !== this.win.name) return true;
      this.win.text += m.data;
      this.win.dirty = true;
      if (m.partial === false) this.win.done = true;
      return false;
    });
    this.queue.push({ ...r, messages: rest });
    this.prompt = r.prompt && !r.prompt.live ? r.prompt : null;
    this.live = (r.live ?? []).filter((p) => this.user.has(p.team) && !this.auto.has(p.team));
    const open = new Set(this.live.map(key));
    for (const k of [...this.answeredLive]) if (!open.has(k)) this.answeredLive.delete(k);
    this.inWindow = !!r.window;
    if (r.tick !== undefined) this.simTick = r.tick;
    this.done = r.done;
    if (r.error) this.feed(`Error: ${r.error}`, "bad");
  }

  // Asks the game to run to `toTick` (between plays), or just on.
  private advanceTo(toTick: number): void {
    if (this.waiting || !this.mode) return;
    this.waiting = true;
    this.send({ type: "advance", to_tick: Math.floor(toTick) });
  }

  // ---------------------------------------------------------------------
  // The per-frame loop: show queued snaps and lines, then the prompt
  // ---------------------------------------------------------------------

  tick(dt: number): void {
    if (!this.mode) return;
    if (this.win) {
      if (!this.windowTick(dt)) return;
      this.win = null;
      this.hideLive();
    }
    if (this.animating) {
      const pb = this.host.playback();
      if (this.between && pb) this.betweenClock(pb);
      if (pb && (pb.playing || !pb.atEnd)) return;
      this.hold -= dt * (pb?.speed ?? 1);
      if (this.hold > 0) return;
      this.animating = false;
      this.between = false;
    }
    while (!this.queue.empty) {
      const step = this.queue.shift()!;
      if (step.kind === "line") {
        this.applyLine(step.line, step.state);
        continue;
      }
      const skip = $<HTMLInputElement>("game-skip").checked;
      // Between plays (the walk back, the huddle, lining up: docs/design/game-flow.md).
      const between = step.name.includes("_between");
      if (between && step.streaming !== undefined && this.mode === "live" && this.user.size > 0) {
        // A live window: it streams in while the play clock runs.
        this.win = { name: step.name, text: step.data, done: !step.streaming, start: 0, playClock: 0, waits: false, shown: false, dirty: true };
        return;
      }
      if (between && (skip || this.host.devMode())) continue;
      if (this.host.show(step.data, `${step.name}.jsonl`)) {
        const pb = this.host.playback()!;
        for (const p of pb.rec.header.players) this.names.set(p.id, p.name);
        if (skip) {
          pb.playing = false;
          pb.seek(pb.lastFrame);
          this.hold = 0.15;
        } else {
          this.hold = between ? 0 : HOLD_AFTER_SNAP; // straight on into the snap
        }
        this.between = between;
        this.animating = true;
        return;
      }
    }
    if (this.prompt) {
      if (this.shownPrompt !== this.prompt) this.showPrompt(this.prompt);
      return;
    }
    if (this.done) {
      this.showMine(false);
      return;
    }
    this.advance();
  }

  // A live window: shows the file as it comes, keeps the game running just
  // ahead of the screen, and puts the user's decisions up. True when it's over.
  private windowTick(dt: number): boolean {
    const w = this.win!;
    const hidden = $<HTMLInputElement>("game-skip").checked || this.host.devMode();
    const deciding = this.live.length > 0;
    const nl = w.text.indexOf("\n");
    if (!w.playClock && nl > 0) {
      const head = JSON.parse(w.text.slice(0, nl));
      w.start = Number(head.details?.start_tick ?? 0);
      w.playClock = Number(head.details?.play_clock ?? 0);
      w.waits = head.details?.waits_for_ready === true;
    }
    let pb = this.host.playback();
    if (!hidden && w.dirty && w.text.includes('"type":"tick"')) {
      // Reload with the new frames, where we were (paused stays paused).
      const paused = w.shown && pb !== null && !pb.playing && !pb.atEnd;
      if (this.host.show(w.text, `${w.name}.jsonl`)) {
        pb = this.host.playback()!;
        for (const p of pb.rec.header.players) this.names.set(p.id, p.name);
        pb.playing = !paused;
        w.shown = true;
      }
      w.dirty = false;
    }
    if (!hidden && w.shown && pb && deciding) pb.speed = 1; // your 40 seconds are real seconds

    // The play clock on the scoreboard.
    let fileTick = this.simTick - w.start;
    if (!hidden && w.shown && pb) {
      this.betweenClock(pb);
      fileTick = pb.tick;
    } else if (w.playClock) {
      const pc = document.getElementById("sb-play-clock");
      const ready = /"tick":(\d+),"event":"ready"/.exec(w.text);
      const left = playClockLeft(w.playClock, (this.simTick - w.start) / 20, w.waits, ready ? Number(ready[1]) / 20 : undefined);
      if (pc) pc.textContent = `:${Math.max(0, Math.ceil(left)).toString().padStart(2, "0")}`;
    }

    // The pace: just ahead of the screen; paused, not at all; hidden, on to
    // the next decision and then in real time while it's open.
    if (this.inWindow && !this.waiting) {
      let want = -1;
      if (hidden) {
        if (!deciding) {
          want = this.simTick + 100000;
          w.hiddenTick = undefined;
        } else {
          w.hiddenTick = (w.hiddenTick ?? this.simTick - w.start) + dt * 20;
          want = w.start + w.hiddenTick;
        }
      } else if (!w.shown || !pb) {
        want = this.simTick + LOOKAHEAD;
      } else if (pb.playing || pb.atEnd) {
        want = w.start + fileTick + LOOKAHEAD;
      }
      if (want > this.simTick) this.advanceTo(want);
    } else if (!this.inWindow && !this.waiting && !w.done) {
      this.advance();
    }

    this.showLive();
    if (!w.done) return false;
    return hidden || !w.shown || !pb || pb.atEnd;
  }

  // The user's open decision between plays, over the field: at the line
  // first, then the call.
  private showLive(): void {
    if (this.prompt) return;
    const order = ["hike", "check", "call_play"];
    const open = this.live
      .filter((p) => !this.answeredLive.has(key(p)))
      .sort((a, b) => order.indexOf(a.input) - order.indexOf(b.input));
    const p = open[0];
    if (!p) {
      this.hideLive();
      return;
    }
    if (!this.shownPrompt || key(this.shownPrompt) !== key(p) || !this.shownPrompt.live) this.showPrompt(p);
    else this.shownPrompt = p; // (the same decision: fresh options, the same menu)
  }

  private hideLive(): void {
    if (this.shownPrompt?.live) this.hidePrompt();
  }

  // The play clock on the scoreboard while the teams get to the line.
  private betweenClock(pb: Playback): void {
    const pc = document.getElementById("sb-play-clock");
    const full = Number(pb.rec.header.details.play_clock ?? 0);
    if (!pc || !full) return;
    const ready = pb.rec.events.find((e) => e.event === "ready");
    const readyAt = ready ? (ready.tick - pb.rec.ticks[0]!) / pb.rec.header.tickRate : undefined;
    const left = playClockLeft(full, pb.seconds, pb.rec.header.details.waits_for_ready === true, readyAt);
    pc.textContent = `:${Math.max(0, Math.ceil(left)).toString().padStart(2, "0")}`;
    pc.classList.toggle("low", left < 5);
  }

  private applyLine(line: any, state: GameState): void {
    this.state = state;
    this.scoreboard(state);
    this.stats.apply(line, state);
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
    const p = this.shownPrompt;
    if (p?.live) {
      // Between plays: it names its decision; the game takes it on its next tick.
      this.lastLive = key(p);
      this.answeredLive.add(this.lastLive);
      this.send({ type: "command", command: { type: "answer", input: p.input, team: p.team, ...fields } });
      this.answering = p;
      this.hidePrompt();
      return;
    }
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
    if (!this.prompt && this.answering && !this.answering.live) this.prompt = this.answering;
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
      case "check":
        this.check(p, body);
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
    const sug = p.suggestion?.play;
    for (const k of (o.kick_plays ?? []) as string[]) {
      tools.append(button(CHOICE_NAMES[k] ?? k, () => this.answer({ play: k }), k === sug ? "suggested" : ""));
    }
    if (o.clock_plays) {
      for (const c of o.clock_plays as string[]) tools.append(button(c === "kneel" ? "Kneel" : "Spike", () => this.answer({ play: c })));
    }
    if (o.offense_personnel) tools.append(el("span", { className: "muted", textContent: `Offense: ${o.offense_personnel} personnel` }));
    tools.append(el("span", { className: "muted", textContent: sug ? `Coach likes: ${CHOICE_NAMES[sug] ?? sug}${p.suggestion.flip ? " (flipped)" : ""}` : "" }));
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

  // The defense at the line: stay with the call, or check to another play in its package.
  private check(p: Prompt, body: HTMLElement): void {
    const o = p.options;
    const sug = p.suggestion?.play;
    const called = (o.plays as PlayOption[]).find((x) => x.id === o.play);
    body.append(
      el("div", { className: "prompt-tools" },
        button(`Stay: ${called?.name ?? o.play}`, () => this.answer({ play: o.play }), sug === o.play ? "suggested" : ""),
        el("span", { className: "muted", textContent: `Offense shows ${o.formation_name ?? o.formation}` }),
      ),
    );
    const grid = el("div", { className: "play-grid" });
    for (const [formation, plays] of byFormation((o.plays as PlayOption[]).filter((x) => x.id !== o.play))) {
      const col = el("div", { className: "play-group" }, el("div", { className: "group-name", textContent: formation }));
      for (const play of plays) {
        const b = button(play.name || play.id, () => this.answer({ play: play.id }), play.id === sug ? "suggested" : "");
        b.title = `${play.id}${play.tags.length ? " · " + play.tags.join(", ") : ""}`;
        col.append(b);
      }
      grid.append(col);
    }
    body.append(grid);
  }

  private hike(p: Prompt, body: HTMLElement): void {
    const o = p.options;
    // The offense is set and the play clock is running (on the scoreboard):
    // snap it when you like. Pause (the transport) stops the clock.
    const hikeBtn = button("Hike (H)", () => this.sendHike(), "hike");
    hikeBtn.id = "hike-button";
    body.append(
      el("div", { className: "prompt-tools" },
        hikeBtn,
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
  }

  private hikeFields: () => Record<string, unknown> = () => ({});

  // Snap it now (with the audible or hot routes picked).
  private sendHike(): void {
    if (this.shownPrompt?.input !== "hike") return;
    this.answer({ snap: true, ...this.hikeFields() });
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
    // Between plays it's taken at once (on the game's next tick).
    this.send({ type: "command", command: { type: "timeout", team } });
    this.feed(`${team === "home" ? "Home" : "Away"} calls timeout`, "note");
  }

  // Between plays: the side hurries to the line (until the snap).
  private hurry(): void {
    const team = this.mySide();
    if (!team || !this.inWindow) return;
    this.send({ type: "command", command: { type: "hurry", team } });
    this.feed(`${team === "home" ? "Home" : "Away"} hurries to the line`, "note");
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
    if (!this.mode && this.plays.key(e)) return true;
    if (e.key === "u" && this.mode === "live" && this.inWindow && this.mySide()) {
      this.hurry();
      return true;
    }
    if (!this.shownPrompt) return false;
    if ((e.key === "h" || e.key === "Enter") && this.shownPrompt.input === "hike") {
      this.sendHike();
      return true;
    }
    return false;
  }
}
