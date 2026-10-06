// Saved sets: whole runs kept in the browser's localStorage under a name, to
// reopen or compare later. An index (one key) lists them; each set's run (its
// info, settings, and every cell's stats and samples) is a key of its own, so
// the list reads without parsing every run. Both carry a version: a set saved
// in another format is ignored rather than misread.
//
// Pure apart from the storage it's given, so it's tested with a fake one.

import type { CellMessage, Domain, MatrixInfo, MatrixSettings, ResultGrid } from "./results";
import { domainOf, titleOf } from "./results";

export const SET_VERSION = 1;
const INDEX_KEY = "cfbsim.builder.sets";
const SET_PREFIX = "cfbsim.builder.set.";

// About what a browser lets one site keep (it counts characters).
export const STORAGE_BUDGET = 5_000_000;

// The part of the Web Storage API a store needs (localStorage, or a fake).
export interface StorageLike {
  getItem(key: string): string | null;
  setItem(key: string, value: string): void;
  removeItem(key: string): void;
}

// A set as the list shows it.
export interface SetMeta {
  id: string;
  name: string;
  saved: string; // ISO time
  title: string; // "Clemson offense vs Clemson 2018"
  kind: Domain;
  rows: number;
  cols: number;
  runs: number;
  cells: number; // cells that had arrived (fewer than rows × cols: a part of a run)
  chars: number; // its size in storage
}

// A set's run, read back.
export interface SetRun {
  meta: SetMeta;
  info: MatrixInfo;
  settings: MatrixSettings;
  cells: CellMessage[];
}

export type SaveResult = { ok: true; meta: SetMeta } | { ok: false; reason: string };

const MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
const two = (n: number) => String(n).padStart(2, "0");

// "Oct 6 02:07", in local time.
export function shortDate(d: Date): string {
  return `${MONTHS[d.getMonth()]} ${d.getDate()} ${two(d.getHours())}:${two(d.getMinutes())}`;
}

// "Clemson vs Clemson 2018 · Oct 6 02:07" (kicks: "Kicks · Clemson vs Clemson 2018 · ...").
export function defaultName(info: MatrixInfo, name: (id: string) => string, when: Date): string {
  const kicks = domainOf(info) === "kicks";
  const a = name((kicks ? info.kicking : info.offense) ?? "");
  const b = kicks ? name(info.receiving ?? "") : info.defense ? name(info.defense) : "empty field";
  return `${kicks ? "Kicks · " : ""}${a} vs ${b} · ${shortDate(when)}`;
}

// "212 KB", "1.4 MB".
export function formatSize(chars: number): string {
  if (chars < 1000) return `${chars} B`;
  if (chars < 1_000_000) return `${Math.round(chars / 1000)} KB`;
  return `${(chars / 1_000_000).toFixed(1)} MB`;
}

function parse(text: string | null): unknown {
  if (text === null) return null;
  try {
    return JSON.parse(text);
  } catch {
    return null;
  }
}

export class SetStore {
  constructor(
    private storage: StorageLike | null,
    private now: () => Date = () => new Date(),
  ) {}

  // The sets, newest first. None without storage, or in another version.
  list(): SetMeta[] {
    const index = parse(this.storage?.getItem(INDEX_KEY) ?? null) as { version?: number; sets?: SetMeta[] } | null;
    if (!index || index.version !== SET_VERSION || !Array.isArray(index.sets)) return [];
    return [...index.sets].sort((a, b) => b.saved.localeCompare(a.saved));
  }

  // The space the sets take, in characters.
  used(): number {
    const index = this.storage?.getItem(INDEX_KEY);
    return (index?.length ?? 0) + this.list().reduce((n, s) => n + s.chars, 0);
  }

  // Keeps the run on screen under `name`. A save that doesn't fit changes nothing.
  save(name: string, grid: ResultGrid, title: string = titleOf(grid.info)): SaveResult {
    if (!this.storage) return { ok: false, reason: "This browser isn't keeping anything for the page (storage is off)." };
    const trimmed = name.trim();
    if (!trimmed) return { ok: false, reason: "Give the set a name." };
    const cells: CellMessage[] = [];
    for (const row of grid.cells) for (const c of row) if (c) cells.push(c);
    const sets = this.list();
    const when = this.now();
    let id = when.getTime().toString(36);
    while (sets.some((s) => s.id === id)) id += "x";
    const body = JSON.stringify({ version: SET_VERSION, info: grid.info, settings: grid.settings, cells });
    const meta: SetMeta = {
      id, name: trimmed, saved: when.toISOString(), title, kind: grid.domain,
      rows: grid.info.rows.length, cols: grid.info.cols.length, runs: grid.info.runs, cells: cells.length,
      chars: body.length + SET_PREFIX.length + id.length,
    };
    const full = () => ({ ok: false as const, reason: `It doesn't fit: the set is ${formatSize(meta.chars)} and the sets already take ${formatSize(this.used())}. Delete some to make room.` });
    try {
      this.storage.setItem(SET_PREFIX + id, body);
    } catch {
      return full();
    }
    if (!this.writeIndex([...sets, meta])) {
      this.storage.removeItem(SET_PREFIX + id);
      return full();
    }
    return { ok: true, meta };
  }

  // A set's run, or null if it's gone or in another version.
  read(id: string): SetRun | null {
    const meta = this.list().find((s) => s.id === id);
    if (!meta) return null;
    const body = parse(this.storage?.getItem(SET_PREFIX + id) ?? null) as
      | { version?: number; info?: MatrixInfo; settings?: MatrixSettings; cells?: CellMessage[] }
      | null;
    if (!body || body.version !== SET_VERSION || !body.info || !Array.isArray(body.cells)) return null;
    return { meta, info: body.info, settings: body.settings ?? {}, cells: body.cells };
  }

  rename(id: string, name: string): boolean {
    const trimmed = name.trim();
    const sets = this.list();
    const s = sets.find((x) => x.id === id);
    if (!s || !trimmed) return false;
    s.name = trimmed;
    return this.writeIndex(sets);
  }

  remove(id: string): void {
    this.storage?.removeItem(SET_PREFIX + id);
    this.writeIndex(this.list().filter((s) => s.id !== id));
  }

  private writeIndex(sets: SetMeta[]): boolean {
    try {
      this.storage?.setItem(INDEX_KEY, JSON.stringify({ version: SET_VERSION, sets }));
      return true;
    } catch {
      return false;
    }
  }
}

// The page's localStorage, or null where it can't be used (blocked, private mode).
export function browserStorage(): StorageLike | null {
  try {
    const s = window.localStorage;
    s.getItem(INDEX_KEY);
    return s;
  } catch {
    return null;
  }
}
