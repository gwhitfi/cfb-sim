// The live game's stats panel (Game panel, under the play-by-play): the box
// score (points by quarter and the scoring summary), the team stats side by
// side, and each team's players (passing, rushing, receiving, and the
// defense's leaders). Built from the box score the server sends with every
// play line (`state.box`, the same JSON as `cfbsim play-game --box`).

import { teamColors } from "./commentary";
import type { GameState, TeamKey } from "./game";

export interface BoxTeam {
  team: string;
  points: number;
  plays: number;
  total_yards: number;
  first_downs: number;
  first_downs_rush: number;
  first_downs_pass: number;
  first_downs_penalty: number;
  rush_att: number;
  rush_yds: number;
  pass_att: number;
  pass_cmp: number;
  pass_yds: number;
  sacks_taken: number;
  interceptions: number;
  fumbles_lost: number;
  third_down: [number, number];
  fourth_down: [number, number];
  penalties: number;
  penalty_yds: number;
  punts: number;
  punt_yds: number;
  kick_returns: [number, number];
  punt_returns: [number, number];
  field_goals: [number, number];
  possession: string;
}

export interface BoxPlayer {
  id: string;
  name: string;
  team: string;
  passing?: { cmp: number; att: number; yds: number; td: number; int: number };
  rushing?: { att: number; yds: number; td: number; long: number };
  receiving?: { rec: number; targets: number; yds: number; td: number; long: number };
  defense?: { tackles: number; sacks: number; int: number; fum_rec: number };
  fumbles?: { fumbles: number; lost: number };
  kicking?: { fg: [number, number]; long: number; xp: [number, number] };
  punting?: { punts: number; yds: number };
  kick_returns?: { att: number; yds: number; td: number };
  punt_returns?: { att: number; yds: number; td: number };
}

export interface Box {
  teams: [BoxTeam, BoxTeam]; // home, away
  periods: [number, number][]; // finished periods: [home, away]
  players: BoxPlayer[];
}

const KEYS: TeamKey[] = ["away", "home"]; // box score order: the visitors on top
const index = (k: TeamKey) => (k === "home" ? 0 : 1);
const yd = (x: number) => Math.round(x);
const avg = (yds: number, n: number) => (n ? (yds / n).toFixed(1) : "0.0");

// ---------------------------------------------------------------------------
// Points by period
// ---------------------------------------------------------------------------

export interface LineScore {
  heads: string[]; // "1".."4", "OT", "2OT", ...
  rows: { key: TeamKey; cells: string[]; total: number }[];
}

// The finished periods, then the one being played (the score minus the
// finished ones), then blanks up to four quarters.
export function lineScore(box: Box, state: GameState, final = false): LineScore {
  const done = box.periods.length;
  const current = final ? done : Math.max(done + 1, state.quarter);
  const n = Math.max(4, current);
  const heads = Array.from({ length: n }, (_, i) => (i < 4 ? String(i + 1) : i === 4 ? "OT" : `${i - 3}OT`));
  const rows = KEYS.map((key) => {
    const t = index(key);
    const played = box.periods.reduce((sum, p) => sum + p[t], 0);
    const cells = heads.map((_, i) => {
      if (i < done) return String(box.periods[i]![t]);
      if (i === done && i < current) return String(state.score[key] - played);
      return "";
    });
    return { key, cells, total: state.score[key] };
  });
  return { heads, rows };
}

// ---------------------------------------------------------------------------
// The scoring summary
// ---------------------------------------------------------------------------

export interface Score {
  quarter: number;
  clock: string;
  team: TeamKey;
  text: string;
  home: number;
  away: number;
}

type Diff = (p: BoxPlayer, before: BoxPlayer | undefined) => boolean;

// The first player whose stat went up between two box scores.
function who(before: Box | null, after: Box, up: Diff): BoxPlayer | undefined {
  const old = new Map((before?.players ?? []).map((p) => [p.id, p]));
  return after.players.find((p) => up(p, old.get(p.id)));
}

const more = (a: number | undefined, b: number | undefined) => (a ?? 0) > (b ?? 0);

export function periodName(q: number): string {
  return q <= 4 ? `Q${q}` : q === 5 ? "OT" : `${q - 4}OT`;
}

// Updates the scoring summary for one game log line (a scoring play, a try).
// `before` is the box score before the line, `after` the one with it.
export function scoring(list: Score[], line: any, state: GameState, before: Box | null, after: Box | null): Score[] {
  if (!after) return list;
  const score = { home: Number(line.home ?? state.score.home), away: Number(line.away ?? state.score.away) };
  if (line.type === "event" && (line.event === "extra_point" || line.event === "two_point_try")) {
    const last = list.at(-1);
    if (!last) return list;
    let note: string;
    if (line.event === "extra_point") {
      const k = who(before, after, (p, b) => more(p.kicking?.xp[1], b?.kicking?.xp[1]));
      note = line.good ? (k ? `${k.name} kick` : "kick good") : "kick failed";
    } else note = line.good ? "two-point try good" : "two-point try failed";
    return [...list.slice(0, -1), { ...last, text: `${last.text} (${note})`, home: score.home, away: score.away }];
  }
  if (line.type !== "play" || line.home === undefined) return list;
  const prev = list.at(-1) ?? { home: 0, away: 0 };
  // A try's play line only shows a score when the defense returns it.
  if (line.try) {
    const team: TeamKey = line.offense === "home" ? "away" : "home";
    return [...list, { quarter: state.quarter, clock: state.clock_text, team, text: "Defensive two-point return", ...score }];
  }
  const team: TeamKey = score.home - prev.home >= score.away - prev.away ? "home" : "away";
  const yards = yd(Number(line.yards ?? 0));
  const receiver = who(before, after, (p, b) => more(p.receiving?.td, b?.receiving?.td));
  const passer = who(before, after, (p, b) => more(p.passing?.td, b?.passing?.td));
  const runner = who(before, after, (p, b) => more(p.rushing?.td, b?.rushing?.td));
  const kr = who(before, after, (p, b) => more(p.kick_returns?.td, b?.kick_returns?.td));
  const pr = who(before, after, (p, b) => more(p.punt_returns?.td, b?.punt_returns?.td));
  const fg = who(before, after, (p, b) => more(p.kicking?.fg[0], b?.kicking?.fg[0]));
  const pick = who(before, after, (p, b) => more(p.defense?.int, b?.defense?.int));
  const recovery = who(before, after, (p, b) => more(p.defense?.fum_rec, b?.defense?.fum_rec));
  const kickYards = /([\d.]+) yd field goal/.exec(String(line.summary ?? ""));
  let text: string;
  if (receiver) text = `${receiver.name} ${yards} yd pass${passer ? ` from ${passer.name}` : ""}`;
  else if (runner) text = `${runner.name} ${yards} yd run`;
  else if (kr) text = `${kr.name} kickoff return`;
  else if (pr) text = `${pr.name} punt return`;
  else if (fg) text = `${fg.name} ${kickYards ? yd(Number(kickYards[1])) : ""} yd field goal`.replace("  ", " ");
  else if (line.outcome === "safety" || score[team] - prev[team] === 2) text = "Safety";
  else if (pick) text = `${pick.name} interception return`;
  else if (recovery) text = `${recovery.name} fumble return`;
  else text = String(line.summary ?? "Score");
  const clock = String(line.clock ?? state.clock_text);
  return [...list, { quarter: Number(line.quarter ?? state.quarter), clock, team, text, ...score }];
}

// ---------------------------------------------------------------------------
// Team stats, side by side
// ---------------------------------------------------------------------------

export interface TeamRow {
  label: string;
  away: string;
  home: string;
  head?: boolean; // a main line (the rest are details under it)
}

export function teamRows(box: Box): TeamRow[] {
  const row = (label: string, f: (t: BoxTeam) => string, head = false): TeamRow => ({
    label,
    away: f(box.teams[1]),
    home: f(box.teams[0]),
    head,
  });
  const frac = (a: [number, number]) => `${a[0]}/${a[1]}`;
  return [
    row("First downs", (t) => String(t.first_downs), true),
    row("Rush · pass · penalty", (t) => `${t.first_downs_rush} · ${t.first_downs_pass} · ${t.first_downs_penalty}`),
    row("Total yards", (t) => String(yd(t.total_yards)), true),
    row("Plays", (t) => String(t.plays)),
    row("Yards per play", (t) => avg(t.total_yards, t.plays)),
    row("Rushing", (t) => `${t.rush_att}-${yd(t.rush_yds)}`, true),
    row("Yards per carry", (t) => avg(t.rush_yds, t.rush_att)),
    row("Passing", (t) => `${t.pass_cmp}/${t.pass_att}, ${yd(t.pass_yds)}`, true),
    row("Yards per attempt", (t) => avg(t.pass_yds, t.pass_att)),
    row("Sacked", (t) => String(t.sacks_taken)),
    row("Third down", (t) => frac(t.third_down), true),
    row("Fourth down", (t) => frac(t.fourth_down)),
    row("Turnovers", (t) => String(t.interceptions + t.fumbles_lost), true),
    row("Interceptions · fumbles", (t) => `${t.interceptions} · ${t.fumbles_lost}`),
    row("Penalties", (t) => `${t.penalties}-${yd(t.penalty_yds)}`, true),
    row("Punts", (t) => (t.punts ? `${t.punts}, ${avg(t.punt_yds, t.punts)} avg` : "0"), true),
    row("Field goals", (t) => frac(t.field_goals)),
    row("Kick returns", (t) => `${t.kick_returns[0]}-${yd(t.kick_returns[1])}`),
    row("Punt returns", (t) => `${t.punt_returns[0]}-${yd(t.punt_returns[1])}`),
    row("Possession", (t) => t.possession, true),
  ];
}

// ---------------------------------------------------------------------------
// A team's players
// ---------------------------------------------------------------------------

export interface Table {
  title: string;
  heads: string[];
  rows: string[][];
}

export interface Leaders {
  label: string;
  text: string;
}

export interface TeamPlayers {
  tables: Table[];
  defense: Leaders[];
  special: Leaders[];
}

// The players on box team `t` (0 home, 1 away). Two teams with the same id
// (a team against itself) can't be told apart; both get everyone.
export function teamPlayers(box: Box, t: 0 | 1, limits = { rush: 4, rec: 5, tackles: 3 }): TeamPlayers {
  const mine = box.players.filter((p) => p.team === box.teams[t].team);
  const by = <T>(get: (p: BoxPlayer) => T | undefined, key: (s: T) => number) =>
    mine.filter((p) => get(p) !== undefined).sort((a, b) => key(get(b)!) - key(get(a)!) || a.name.localeCompare(b.name));
  const tables: Table[] = [];
  const passers = by((p) => p.passing, (s) => s.att * 1000 + s.yds);
  if (passers.length) {
    tables.push({
      title: "Passing",
      heads: ["C/A", "Yds", "TD", "Int"],
      rows: passers.map((p) => {
        const s = p.passing!;
        return [p.name, `${s.cmp}/${s.att}`, String(yd(s.yds)), String(s.td), String(s.int)];
      }),
    });
  }
  const runners = by((p) => p.rushing, (s) => s.yds).slice(0, limits.rush);
  if (runners.length) {
    tables.push({
      title: "Rushing",
      heads: ["Car", "Yds", "Avg", "TD", "Lg"],
      rows: runners.map((p) => {
        const s = p.rushing!;
        return [p.name, String(s.att), String(yd(s.yds)), avg(s.yds, s.att), String(s.td), String(yd(s.long))];
      }),
    });
  }
  const catchers = by((p) => p.receiving, (s) => s.yds * 100 + s.rec).slice(0, limits.rec);
  if (catchers.length) {
    tables.push({
      title: "Receiving",
      heads: ["Rec", "Tgt", "Yds", "TD", "Lg"],
      rows: catchers.map((p) => {
        const s = p.receiving!;
        return [p.name, String(s.rec), String(s.targets), String(yd(s.yds)), String(s.td), String(yd(s.long))];
      }),
    });
  }
  const list = (players: BoxPlayer[], n: (p: BoxPlayer) => number) =>
    players.map((p) => `${p.name} ${n(p)}`).join(" · ");
  const defense: Leaders[] = [];
  const stat = (label: string, n: (p: BoxPlayer) => number, limit = 99) => {
    const top = mine.filter((p) => n(p) > 0).sort((a, b) => n(b) - n(a) || a.name.localeCompare(b.name)).slice(0, limit);
    if (top.length) defense.push({ label, text: list(top, n) });
  };
  stat("Tackles", (p) => p.defense?.tackles ?? 0, limits.tackles);
  stat("Sacks", (p) => p.defense?.sacks ?? 0);
  stat("Interceptions", (p) => p.defense?.int ?? 0);
  stat("Fumble recoveries", (p) => p.defense?.fum_rec ?? 0);
  const special: Leaders[] = [];
  for (const p of mine) {
    const k = p.kicking;
    if (k) {
      const fg = k.fg[1] ? `FG ${k.fg[0]}/${k.fg[1]}${k.fg[0] ? `, long ${yd(k.long)}` : ""}` : "";
      const xp = k.xp[1] ? `XP ${k.xp[0]}/${k.xp[1]}` : "";
      special.push({ label: "Kicking", text: `${p.name} ${[fg, xp].filter(Boolean).join(" · ")}` });
    }
    if (p.punting) {
      const s = p.punting;
      special.push({ label: "Punting", text: `${p.name} ${s.punts}, ${avg(s.yds, s.punts)} avg` });
    }
  }
  const returns = (label: string, get: (p: BoxPlayer) => { att: number; yds: number; td: number } | undefined) => {
    const r = by(get, (s) => s.yds);
    if (r.length) {
      special.push({
        label,
        text: r.map((p) => `${p.name} ${get(p)!.att}-${yd(get(p)!.yds)}${get(p)!.td ? `, ${get(p)!.td} TD` : ""}`).join(" · "),
      });
    }
  };
  returns("Kick returns", (p) => p.kick_returns);
  returns("Punt returns", (p) => p.punt_returns);
  return { tables, defense, special };
}

// ---------------------------------------------------------------------------
// The panel
// ---------------------------------------------------------------------------

const OPEN_KEY = "cfbsim.stats.open";

function el<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  props: Record<string, unknown> = {},
  ...children: (Node | string)[]
): HTMLElementTagNameMap[K] {
  const e = document.createElement(tag);
  Object.assign(e, props);
  e.append(...children);
  return e;
}

export class StatsPanel {
  private box: Box | null = null;
  private scores: Score[] = [];
  private state: GameState | null = null;
  private final = false;
  private open: Record<string, boolean> = {};

  constructor(private root: HTMLElement) {
    try {
      this.open = JSON.parse(localStorage.getItem(OPEN_KEY) ?? "{}") ?? {};
    } catch {
      this.open = {};
    }
  }

  clear(): void {
    this.box = null;
    this.scores = [];
    this.state = null;
    this.final = false;
    this.root.replaceChildren(el("div", { className: "muted", textContent: "Stats show after the first play." }));
  }

  // One game log line and the state after it.
  apply(line: any, state: GameState & { box?: Box }): void {
    this.state = state;
    if (line.type === "final") this.final = true;
    if (!state.box) return;
    this.scores = scoring(this.scores, line, state, this.box, state.box);
    this.box = state.box;
    this.render();
  }

  private render(): void {
    const box = this.box;
    const s = this.state;
    if (!box || !s) return;
    const color = (k: TeamKey) => teamColors(s.teams[k].color ?? "#444444");
    const badge = (k: TeamKey) => {
      const c = color(k);
      return el("span", { className: "st-badge", textContent: s.teams[k].abbr, style: `background:${c.base};color:${c.ink}` });
    };
    const sections = [
      this.section("box", "Box score", this.boxScore(box, s, badge)),
      this.section("team", "Team stats", this.teamStats(box, badge)),
      ...KEYS.map((k) => this.section(`players-${k}`, `${s.teams[k].name}`, this.players(box, index(k) as 0 | 1), badge(k))),
    ];
    this.root.replaceChildren(...sections);
  }

  private section(key: string, title: string, body: Node, tag?: Node): HTMLDetailsElement {
    const d = el("details", { className: "st-section", open: this.open[key] ?? true });
    const summary = el("summary", {}, ...(tag ? [tag, " "] : []), title);
    d.append(summary, body);
    d.addEventListener("toggle", () => {
      this.open[key] = d.open;
      try {
        localStorage.setItem(OPEN_KEY, JSON.stringify(this.open));
      } catch {
        // Not remembered.
      }
    });
    return d;
  }

  private boxScore(box: Box, s: GameState, badge: (k: TeamKey) => Node): Node {
    const ls = lineScore(box, s, this.final);
    const table = el(
      "table",
      { className: "st-table st-line" },
      el("thead", {}, el("tr", {}, el("th"), ...ls.heads.map((h) => el("th", { textContent: h })), el("th", { textContent: "T" }))),
      el(
        "tbody",
        {},
        ...ls.rows.map((r) =>
          el("tr", {}, el("td", {}, badge(r.key)), ...r.cells.map((c) => el("td", { textContent: c })), el("td", { className: "st-total", textContent: String(r.total) })),
        ),
      ),
    );
    const list = el("ol", { className: "st-scores" });
    let quarter = 0;
    for (const sc of this.scores) {
      if (sc.quarter !== quarter) {
        quarter = sc.quarter;
        list.append(el("li", { className: "st-period", textContent: periodName(quarter) }));
      }
      list.append(
        el(
          "li",
          {},
          badge(sc.team),
          el("span", { className: "st-clock", textContent: sc.clock }),
          el("span", { className: "st-play", textContent: sc.text }),
          el("span", { className: "st-after", textContent: `${sc.away}-${sc.home}` }),
        ),
      );
    }
    if (!this.scores.length) list.append(el("li", { className: "muted", textContent: "No scoring yet" }));
    return el("div", {}, table, list);
  }

  private teamStats(box: Box, badge: (k: TeamKey) => Node): Node {
    return el(
      "table",
      { className: "st-table st-team" },
      el("thead", {}, el("tr", {}, el("th"), el("th", {}, badge("away")), el("th", {}, badge("home")))),
      el(
        "tbody",
        {},
        ...teamRows(box).map((r) =>
          el("tr", { className: r.head ? "head" : "sub" }, el("td", { textContent: r.label }), el("td", { textContent: r.away }), el("td", { textContent: r.home })),
        ),
      ),
    );
  }

  private players(box: Box, t: 0 | 1): Node {
    const p = teamPlayers(box, t);
    const out = el("div", { className: "st-players" });
    for (const tbl of p.tables) {
      out.append(
        el(
          "table",
          { className: "st-table st-ind" },
          el("thead", {}, el("tr", {}, el("th", { textContent: tbl.title }), ...tbl.heads.map((h) => el("th", { textContent: h })))),
          el("tbody", {}, ...tbl.rows.map((r) => el("tr", {}, ...r.map((c) => el("td", { textContent: c }))))),
        ),
      );
    }
    const leaders = (title: string, list: Leaders[]) => {
      if (!list.length) return;
      out.append(
        el(
          "div",
          { className: "st-leaders" },
          el("div", { className: "st-sub", textContent: title }),
          ...list.map((l) => el("div", { className: "st-leader" }, el("span", { textContent: l.label }), el("span", { textContent: l.text }))),
        ),
      );
    };
    leaders("Defense", p.defense);
    leaders("Special teams", p.special);
    if (!out.childElementCount) out.append(el("div", { className: "muted", textContent: "No stats yet" }));
    return out;
  }
}
