// Plays mode: run any play through cfbsim-server and watch it. The bar along
// the bottom has the offense's plays on the left and the defense's on the
// right, grouped by formation like the in-game play call, and the seed in the
// middle. Clicking a play runs it; the arrows (or , and .) step the seed and
// run; P reruns. The server reloads data/ on every run, so after editing a
// tuning file, P shows the change on the same seed.
//
// The Kicks switch swaps the sides for special teams: the kicking team's
// kickoffs, punts, and field goals on the left, and on the right the
// receiving team's returns (or blocks) for the kind of kick picked.

export interface PlayInfo {
  id: string;
  name: string;
  type: string; // "pass" | "run" | "defense" | "kickoff" | "punt" | "field_goal" | "kickoff_return" | ...
  formation: string;
  formation_name?: string;
  tags?: string[];
}

export interface TeamInfo {
  id: string;
  name: string;
  abbr: string;
  offense?: PlayInfo[];
  defense?: PlayInfo[];
  kicking?: PlayInfo[];
  receiving?: PlayInfo[];
}

export type PlayMode = "scrimmage" | "kicks";

// The form's values (also what's remembered between visits, per mode). In
// kicks mode the "offense" is the kicking team and its kick, the "defense"
// the receiving team and its return.
export interface PlayForm {
  mode?: PlayMode; // missing: scrimmage (forms saved before kicks existed)
  offense: string;
  play: string;
  defense: string; // "" = an empty field (scrimmage only)
  defPlay: string;
  seed: number;
  hash: string;
  yardLine: number; // the offense's (kicking team's) own yard line
  flip: boolean;
  huddle: boolean;
}

export interface PlayReply {
  summary: string;
  outcome: string;
  yards: number;
  tracking: string;
  file?: string;
}

const FORM_KEY: Record<PlayMode, string> = { scrimmage: "cfbsim.play", kicks: "cfbsim.kick" };
const MODE_KEY = "cfbsim.plays.mode";

// The receiving team's answer to each kind of kick.
const ANSWERS: Record<string, string> = {
  kickoff: "kickoff_return",
  punt: "punt_return",
  field_goal: "field_goal_block",
};

// Where each kind of kick is from by default (the kicking team's own yard line).
const KICK_YARD: Record<string, number> = { kickoff: 35, punt: 30, field_goal: 70 };

// The receiving team's plays that answer a kind of kick.
export function answersTo(kickType: string, plays: PlayInfo[]): PlayInfo[] {
  return plays.filter((p) => p.type === ANSWERS[kickType]);
}

export function kickYardLine(kickType: string): number {
  return KICK_YARD[kickType] ?? 35;
}

// The server message for a form: run_play, or run_kick in kicks mode.
export function playRequest(f: PlayForm): Record<string, unknown> {
  if (f.mode === "kicks") {
    return {
      type: "run_kick",
      kicking: f.offense,
      kick: f.play,
      receiving: f.defense,
      return: f.defPlay,
      seed: f.seed,
      hash: f.hash,
      yard_line: f.yardLine,
    };
  }
  const msg: Record<string, unknown> = {
    type: "run_play",
    offense: f.offense,
    play: f.play,
    seed: f.seed,
    hash: f.hash,
    yard_line: f.yardLine,
    flip: f.flip,
    huddle: f.huddle,
  };
  if (f.defense) {
    msg.defense = f.defense;
    msg.def_play = f.defPlay;
  }
  return msg;
}

// Seeds are whole numbers from 1 up.
export function stepSeed(seed: number, step: number): number {
  const s = Number.isFinite(seed) ? Math.round(seed) : 1;
  return Math.max(1, s + step);
}

// A name for the loaded play. The same play and seed keep the same name, so a
// rerun keeps the moment you were looking at (like reloading a file).
export function playTitle(f: PlayForm, teams: TeamInfo[]): string {
  const kicks = f.mode === "kicks";
  const team = (id: string) => teams.find((t) => t.id === id);
  const play = (t: TeamInfo | undefined, list: "offense" | "defense" | "kicking" | "receiving", id: string) =>
    t?.[list]?.find((p) => p.id === id)?.name ?? id;
  const o = team(f.offense);
  const d = f.defense ? team(f.defense) : undefined;
  const offense = `${o?.abbr ?? f.offense} ${play(o, kicks ? "kicking" : "offense", f.play)}`;
  const defense = f.defense
    ? `${d?.abbr ?? f.defense} ${play(d, kicks ? "receiving" : "defense", f.defPlay)}`
    : "empty field";
  return `${offense} vs ${defense} · seed ${f.seed}`;
}

// Plays grouped by formation, in playbook order (as in the game's play call).
export function groupByFormation(plays: PlayInfo[]): [string, PlayInfo[]][] {
  const groups = new Map<string, PlayInfo[]>();
  for (const p of plays) {
    const key = p.formation_name ?? p.formation;
    if (!groups.has(key)) groups.set(key, []);
    groups.get(key)!.push(p);
  }
  return [...groups.entries()];
}

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;

export interface PlaysHost {
  send(msg: unknown): void;
  // Opens the server connection if it isn't open.
  connect(): void;
  show(text: string, name: string): boolean;
}

export class PlayRunner {
  private teams: TeamInfo[] = [];
  private pending: PlayForm | null = null;
  private mode: PlayMode = "scrimmage";
  private play = "";
  private defPlay = "";

  constructor(private host: PlaysHost) {
    $("plays-toggle").addEventListener("click", () => this.toggle());
    $("play-offense").addEventListener("change", () => this.buildGrids());
    $("play-defense").addEventListener("change", () => this.buildGrids());
    $("play-prev").addEventListener("click", () => this.run(-1));
    $("play-next").addEventListener("click", () => this.run(1));
    $("play-run").addEventListener("click", () => this.run());
    $("play-random").addEventListener("click", () => {
      $<HTMLInputElement>("play-seed").value = String(1 + Math.floor(Math.random() * 99999));
      this.run();
    });
    $<HTMLInputElement>("play-seed").addEventListener("change", () => this.run());
    $("play-mode-plays").addEventListener("click", () => this.setMode("scrimmage"));
    $("play-mode-kicks").addEventListener("click", () => this.setMode("kicks"));
    try {
      if (localStorage.getItem(MODE_KEY) === "kicks") this.mode = "kicks";
      // Reopen it if it was open (after the caller has finished setting up).
      if (localStorage.getItem("cfbsim.plays.open") === "1") queueMicrotask(() => this.toggle(true));
    } catch {
      // No storage: starts closed.
    }
    this.showMode();
  }

  get open(): boolean {
    return !$("plays-bar").hidden;
  }

  get ready(): boolean {
    return this.open && this.teams.length > 0;
  }

  toggle(show = !this.open): void {
    $("plays-bar").hidden = !show;
    $("plays-toggle").classList.toggle("active", show);
    try {
      localStorage.setItem("cfbsim.plays.open", show ? "1" : "0");
    } catch {
      // Not remembered.
    }
    if (show) {
      if (!this.teams.length) this.result("Connecting to cfbsim-server…");
      this.host.connect();
    }
    window.dispatchEvent(new Event("resize")); // the field gets shorter or taller
  }

  // The connection's state, shown until there's a play to show.
  serverStatus(text: string, bad: boolean): void {
    if (bad || !this.teams.length) this.result(text, bad);
  }

  // The teams and playbooks from the server's hello.
  setTeams(teams: TeamInfo[]): void {
    const first = this.teams.length === 0;
    this.teams = teams;
    this.fill(first ? this.remembered(this.mode) : this.form(), first);
    if (first) this.result("Pick a play, or press P to run this one");
  }

  // Keys while no game is on: , and . step the seed and run, P reruns.
  key(e: KeyboardEvent): boolean {
    if (!this.ready || e.ctrlKey || e.metaKey || e.altKey) return false;
    if (e.key === ",") this.run(-1);
    else if (e.key === ".") this.run(1);
    else if (e.key === "p") this.run();
    else return false;
    return true;
  }

  // The server's reply to run_play or run_kick.
  onResult(m: PlayReply): void {
    const f = this.pending ?? this.form();
    this.pending = null;
    this.result(m.summary);
    this.host.show(m.tracking, playTitle(f, this.teams));
  }

  onError(message: string): boolean {
    if (!this.pending) return false;
    this.pending = null;
    this.result(message, true);
    return true;
  }

  // Scrimmage or kicks: each keeps its own teams, plays, and yard line.
  private setMode(mode: PlayMode): void {
    if (mode === this.mode) return;
    this.save(this.form());
    this.mode = mode;
    try {
      localStorage.setItem(MODE_KEY, mode);
    } catch {
      // Not remembered.
    }
    this.showMode();
    if (this.teams.length) this.fill(this.remembered(mode), true);
  }

  // The labels and options for the mode.
  private showMode(): void {
    const kicks = this.mode === "kicks";
    $("play-mode-plays").classList.toggle("active", !kicks);
    $("play-mode-kicks").classList.toggle("active", kicks);
    $("play-offense-tag").textContent = kicks ? "Kicking" : "Offense";
    $("play-defense-tag").textContent = kicks ? "Receiving" : "Defense";
    $("play-scrimmage-options").hidden = kicks;
    $("play-yard-label").textContent = kicks ? "Kicking team's own" : "Own yard line";
  }

  // Sets the team menus (and, with `all`, every option) from a form.
  private fill(saved: PlayForm | null, all: boolean): void {
    const kicks = this.mode === "kicks";
    const teams = this.teams;
    const off = $<HTMLSelectElement>("play-offense");
    const def = $<HTMLSelectElement>("play-defense");
    const options = teams.map((t) => new Option(t.name, t.id));
    off.replaceChildren(...options);
    // Kicks always have a receiving team; a play can run against an empty field.
    def.replaceChildren(
      ...(kicks ? [] : [new Option("Empty field", "")]),
      ...teams.map((t) => new Option(t.name, t.id)),
    );
    if (saved && teams.some((t) => t.id === saved.offense)) off.value = saved.offense;
    if (saved && ((!kicks && saved.defense === "") || teams.some((t) => t.id === saved.defense))) def.value = saved.defense;
    else if (teams.length > 1) def.value = teams[1]!.id;
    this.play = saved?.play ?? "";
    this.defPlay = saved?.defPlay ?? "";
    if (saved && all) {
      $<HTMLInputElement>("play-seed").value = String(saved.seed);
      $<HTMLSelectElement>("play-hash").value = saved.hash;
      $<HTMLInputElement>("play-yard").value = String(saved.yardLine);
      $<HTMLInputElement>("play-flip").checked = saved.flip;
      $<HTMLInputElement>("play-huddle").checked = saved.huddle;
    }
    this.buildGrids();
    if (kicks && !saved && all) $<HTMLInputElement>("play-yard").value = String(kickYardLine(this.kickType()));
  }

  private run(step = 0): void {
    if (!this.teams.length) return;
    const seed = $<HTMLInputElement>("play-seed");
    if (step) seed.value = String(stepSeed(Number(seed.value), step));
    const f = this.form();
    if (!f.play || (f.defense && !f.defPlay) || (f.mode === "kicks" && !f.defPlay)) {
      this.result(f.mode === "kicks" && f.play ? "The receiving team has no return for this kick" : "Pick a play", true);
      return;
    }
    this.save(f);
    this.pending = f;
    this.result(`Running seed ${f.seed}…`);
    this.host.send(playRequest(f));
  }

  // The plays for each side in this mode.
  private lists(): { off: PlayInfo[]; def: PlayInfo[]; defId: string } {
    const team = (id: string) => this.teams.find((t) => t.id === id);
    const o = team($<HTMLSelectElement>("play-offense").value);
    const defId = $<HTMLSelectElement>("play-defense").value;
    const d = defId ? team(defId) : undefined;
    if (this.mode === "kicks") return { off: o?.kicking ?? [], def: d?.receiving ?? [], defId };
    return { off: o?.offense ?? [], def: d?.defense ?? [], defId };
  }

  // The picked kick's type (kicks mode).
  private kickType(): string {
    return this.lists().off.find((p) => p.id === this.play)?.type ?? "kickoff";
  }

  // Both play grids, for the teams picked; keeps the picks where it can.
  private buildGrids(): void {
    const kicks = this.mode === "kicks";
    const { off, def: all, defId } = this.lists();
    if (!off.some((p) => p.id === this.play)) this.play = off[0]?.id ?? "";
    // Kicks: only the receiving team's answers to the kick picked.
    const def = kicks ? answersTo(this.kickType(), all) : all;
    if (!def.some((p) => p.id === this.defPlay)) this.defPlay = def[0]?.id ?? "";
    this.grid("play-offense-grid", off, this.play, (id) => {
      const before = this.kickType();
      this.play = id;
      if (!kicks) return;
      // A different kind of kick: its own spot, and the returns for it.
      const type = this.kickType();
      if (type !== before) {
        $<HTMLInputElement>("play-yard").value = String(kickYardLine(type));
        this.buildGrids();
      }
    });
    if (defId) {
      this.grid("play-defense-grid", def, this.defPlay, (id) => (this.defPlay = id));
    } else {
      $("play-defense-grid").replaceChildren(Object.assign(document.createElement("div"), {
        className: "muted",
        textContent: "No defense: the offense runs against an empty field.",
      }));
    }
  }

  private grid(id: string, plays: PlayInfo[], picked: string, pick: (id: string) => void): void {
    // One section per formation: its name, then its plays in an even grid.
    const sections = groupByFormation(plays).map(([formation, list]) => {
      const section = document.createElement("section");
      section.className = "formation";
      const head = document.createElement("div");
      head.className = "group-name";
      head.textContent = formation;
      const buttons = document.createElement("div");
      buttons.className = "formation-plays";
      section.append(head, buttons);
      for (const p of list) {
        const b = document.createElement("button");
        b.textContent = p.name || p.id;
        b.title = `${p.name || p.id}\n${p.id} · ${p.type}${p.tags?.length ? " · " + p.tags.join(", ") : ""}`;
        b.classList.toggle("picked", p.id === picked);
        if (p.type === "run") b.classList.add("run");
        b.addEventListener("click", () => {
          pick(p.id);
          // (A pick that rebuilt the grids has marked the new buttons already.)
          if (b.isConnected) {
            for (const other of $(id).querySelectorAll("button")) other.classList.toggle("picked", other === b);
          }
          this.run();
        });
        buttons.append(b);
      }
      return section;
    });
    $(id).replaceChildren(...sections);
  }

  private form(): PlayForm {
    const defense = $<HTMLSelectElement>("play-defense").value;
    return {
      mode: this.mode,
      offense: $<HTMLSelectElement>("play-offense").value,
      play: this.play,
      defense,
      defPlay: defense ? this.defPlay : "",
      seed: stepSeed(Number($<HTMLInputElement>("play-seed").value), 0),
      hash: $<HTMLSelectElement>("play-hash").value,
      yardLine: Math.min(99, Math.max(1, Number($<HTMLInputElement>("play-yard").value) || 25)),
      flip: $<HTMLInputElement>("play-flip").checked,
      huddle: $<HTMLInputElement>("play-huddle").checked,
    };
  }

  private save(f: PlayForm): void {
    try {
      localStorage.setItem(FORM_KEY[f.mode ?? "scrimmage"], JSON.stringify(f));
    } catch {
      // Storage blocked: the form just isn't remembered.
    }
  }

  private remembered(mode: PlayMode): PlayForm | null {
    try {
      const text = localStorage.getItem(FORM_KEY[mode]);
      return text ? (JSON.parse(text) as PlayForm) : null;
    } catch {
      return null;
    }
  }

  private result(text: string, bad = false): void {
    const r = $("play-result");
    r.textContent = text;
    r.classList.toggle("bad", bad);
  }
}
