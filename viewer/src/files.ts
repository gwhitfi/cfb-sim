// Replays: titles for the files the Replays menu lists. Play replays (one
// play's tracking file) come from the dev server's file list; game replays
// (game logs) from cfbsim-server. The menu shows a title and keeps the path
// for the tooltip.

import type { TeamInfo } from "./plays";

// The folders each kind of replay is saved in.
export const PLAY_FOLDER = "out/plays";
export const GAME_FOLDER = "out/games";

export interface FileTitle {
  title: string;
  sub: string; // a second, dimmer line ("" for none)
}

// "clemson_2018" -> "Clemson 2018", for ids with no name to look up.
export function humanize(id: string): string {
  return id
    .split(/[_\s]+/)
    .filter(Boolean)
    .map((w) => w[0]!.toUpperCase() + w.slice(1))
    .join(" ");
}

const base = (path: string) => path.split("/").pop()!;
const teamName = (id: string, teams: TeamInfo[]) => teams.find((t) => t.id === id)?.name ?? humanize(id);

// Splits "<team id>_<play id>" with the team ids the server knows (longest first).
function splitTeamPlay(s: string, teams: TeamInfo[]): [TeamInfo, string] | null {
  const sorted = [...teams].sort((a, b) => b.id.length - a.id.length);
  const t = sorted.find((t) => s.startsWith(`${t.id}_`));
  return t ? [t, s.slice(t.id.length + 1)] : null;
}

// A play saved by Plays mode: "<off>_<play>_vs_<def>_<def play>_seed<N>.jsonl"
// (or "..._vs_empty_seed<N>"). Anything else: the file's name, made readable.
export function playFileTitle(path: string, teams: TeamInfo[] = []): FileTitle {
  const name = base(path).replace(/\.jsonl$/, "");
  const m = /^(.+)_vs_(.+)_seed(\d+)$/.exec(name);
  const off = m && splitTeamPlay(m[1]!, teams);
  const def = m && m[2] !== "empty" ? splitTeamPlay(m[2]!, teams) : null;
  if (m && off && (def || m[2] === "empty")) {
    const playName = (t: TeamInfo, list: "offense" | "defense", id: string) =>
      t[list]?.find((p) => p.id === id)?.name ?? humanize(id);
    const offPlay = playName(off[0], "offense", off[1]);
    const defPlay = def ? playName(def[0], "defense", def[1]) : "empty field";
    const who = def ? `${off[0].name} vs ${def[0].name}` : off[0].name;
    return { title: `${offPlay} vs ${defPlay}`, sub: `${who} · seed ${m[3]}` };
  }
  if (m) return { title: `${humanize(m[1]!)} vs ${humanize(m[2]!)}`, sub: `seed ${m[3]}` };
  return { title: humanize(name), sub: "" };
}

const MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];

// A game log saved by the server: "<home>_vs_<away>_<YYYYMMDD>_<HHMMSS>_seed<N>.gamelog.jsonl".
export function gameLogTitle(path: string, teams: TeamInfo[] = []): FileTitle {
  const name = base(path).replace(/\.gamelog\.jsonl$/, "");
  const m = /^(.+)_vs_(.+)_(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})\d{2}_seed(\d+)$/.exec(name);
  if (!m) return { title: humanize(name), sub: "" };
  const [, home, away, y, mo, d, h, mi, seed] = m;
  const hour = Number(h);
  const time = `${hour % 12 || 12}:${mi} ${hour < 12 ? "AM" : "PM"}`;
  const date = `${MONTHS[Number(mo) - 1] ?? mo} ${Number(d)}, ${y}`;
  return { title: `${teamName(away!, teams)} at ${teamName(home!, teams)}`, sub: `${date} ${time} · seed ${seed}` };
}

// Tracking files grouped by folder: the play replays folder first, then the
// rest in the order given ("." is the repo root).
export function groupByFolder(paths: string[]): [string, string[]][] {
  const groups = new Map<string, string[]>([[PLAY_FOLDER, []]]);
  for (const p of paths) {
    const i = p.lastIndexOf("/");
    const dir = i < 0 ? "." : p.slice(0, i);
    if (!groups.has(dir)) groups.set(dir, []);
    groups.get(dir)!.push(p);
  }
  return [...groups.entries()].filter(([, files]) => files.length > 0);
}

// Game logs, newest first (the names start with the teams, so sort on the time stamp).
export function newestFirst(paths: string[]): string[] {
  const stamp = (p: string) => /_(\d{8}_\d{6})_seed/.exec(p)?.[1] ?? "";
  return [...paths].sort((a, b) => stamp(b).localeCompare(stamp(a)) || a.localeCompare(b));
}
