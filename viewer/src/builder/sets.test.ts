import { describe, expect, it } from "vitest";
import { ResultGrid, type CellStats, type MatrixInfo } from "./results";
import { SET_VERSION, SetStore, defaultName, formatSize, shortDate, type StorageLike } from "./sets";

// A storage that refuses to hold more than `quota` characters, like a browser.
class FakeStorage implements StorageLike {
  items = new Map<string, string>();
  constructor(public quota = Infinity) {}
  getItem(key: string) {
    return this.items.get(key) ?? null;
  }
  setItem(key: string, value: string) {
    let used = key.length + value.length;
    for (const [k, v] of this.items) if (k !== key) used += k.length + v.length;
    if (used > this.quota) throw new Error("QuotaExceededError");
    this.items.set(key, value);
  }
  removeItem(key: string) {
    this.items.delete(key);
  }
}

const info: MatrixInfo = {
  offense: "clemson_2025", defense: "clemson_2018", runs: 10, seed: 1,
  rows: [{ id: "smash", name: "Smash", type: "pass" }, { id: "iz", name: "Inside Zone", type: "run" }],
  cols: [{ id: "c2", name: "Cover 2" }],
};
const stats = { plays: 10, complete: 6, picks: 0, sacks: 0, scrambles: 0, success: 5, pressured: 2, fumbles_lost: 0, gaining: 10,
  gained_yards: 60, gained_yards_sq: 500, completed_yards: 60, yac: 20, nonpositive: 1, ten_plus: 3, twenty_plus: 1 } satisfies CellStats;

function grid(cells = 2): ResultGrid {
  const g = new ResultGrid(info, { offense: "clemson_2025", defense: "clemson_2018", yard_line: 40 });
  for (let r = 0; r < cells; r++) g.add({ row: r, col: 0, stats, samples: [{ kind: "best", run: 3, seed: 4, hash: "left", flip: false, yards: 12 }] });
  return g;
}

let clock = new Date(2026, 9, 6, 2, 7).getTime();
const tick = () => new Date((clock += 60_000));

describe("names and sizes", () => {
  it("name a set by its teams and the time", () => {
    const names: Record<string, string> = { clemson_2025: "Clemson", clemson_2018: "Clemson 2018" };
    const when = new Date(2026, 9, 6, 2, 7);
    expect(shortDate(when)).toBe("Oct 6 02:07");
    expect(defaultName(info, (id) => names[id] ?? id, when)).toBe("Clemson vs Clemson 2018 · Oct 6 02:07");
    expect(defaultName({ ...info, defense: "" }, (id) => id, when)).toBe("clemson_2025 vs empty field · Oct 6 02:07");
    expect(defaultName({ ...info, kind: "kicks", kicking: "a", receiving: "b" }, (id) => id, when)).toBe("Kicks · a vs b · Oct 6 02:07");
  });

  it("print a size", () => {
    expect(formatSize(512)).toBe("512 B");
    expect(formatSize(212_400)).toBe("212 KB");
    expect(formatSize(1_420_000)).toBe("1.4 MB");
  });
});

describe("SetStore", () => {
  it("saves a run and reads back its info, settings, and every cell", () => {
    const store = new SetStore(new FakeStorage(), tick);
    const saved = store.save("  First  ", grid());
    expect(saved.ok).toBe(true);
    const [meta] = store.list();
    expect(meta).toMatchObject({ name: "First", kind: "scrimmage", rows: 2, cols: 1, runs: 10, cells: 2 });
    const back = store.read(meta!.id)!;
    expect(back.info).toEqual(info);
    expect(back.settings.yard_line).toBe(40);
    expect(back.cells).toHaveLength(2);
    expect(back.cells[0]!.samples![0]!.seed).toBe(4);
    expect(back.cells[1]!.stats).toEqual(stats);
  });

  it("keeps a part of a run, counting the cells that had arrived", () => {
    const store = new SetStore(new FakeStorage(), tick);
    store.save("Part", grid(1));
    expect(store.list()[0]!.cells).toBe(1);
  });

  it("lists the newest first, renames, and deletes", () => {
    const storage = new FakeStorage();
    const store = new SetStore(storage, tick);
    store.save("Old", grid());
    store.save("New", grid());
    expect(store.list().map((s) => s.name)).toEqual(["New", "Old"]);
    const old = store.list()[1]!;
    expect(store.rename(old.id, "Renamed")).toBe(true);
    expect(store.rename(old.id, "  ")).toBe(false);
    expect(store.list().map((s) => s.name)).toEqual(["New", "Renamed"]);
    store.remove(old.id);
    expect(store.list().map((s) => s.name)).toEqual(["New"]);
    expect(store.read(old.id)).toBeNull();
    expect([...storage.items.keys()].filter((k) => k.startsWith("cfbsim.builder.set."))).toHaveLength(1);
  });

  it("refuses a save that doesn't fit and changes nothing", () => {
    const storage = new FakeStorage();
    const store = new SetStore(storage, tick);
    store.save("Fits", grid());
    const before = new Map(storage.items);
    storage.quota = store.used() + 100;
    const tight = new SetStore(storage, tick);
    const r = tight.save("Too big", grid());
    expect(r.ok).toBe(false);
    if (!r.ok) expect(r.reason).toMatch(/doesn't fit/);
    expect(storage.items).toEqual(before);
    expect(tight.list().map((s) => s.name)).toEqual(["Fits"]);
  });

  it("undoes the run when the index then doesn't fit", () => {
    const storage = new FakeStorage();
    const store = new SetStore(storage, tick);
    store.save("Fits", grid());
    const body = [...storage.items].find(([k]) => k.startsWith("cfbsim.builder.set."))![1];
    // Room for a second run, but not for the longer index after it.
    storage.quota = store.used() + body.length + 20;
    const keys = [...storage.items.keys()];
    expect(store.save("Second", grid()).ok).toBe(false);
    expect([...storage.items.keys()]).toEqual(keys);
  });

  it("measures the space used", () => {
    const store = new SetStore(new FakeStorage(), tick);
    expect(store.used()).toBe(0);
    store.save("A", grid());
    const one = store.used();
    store.save("B", grid());
    expect(store.used()).toBeGreaterThan(one * 1.8);
  });

  it("ignores sets in another version and works without storage", () => {
    const storage = new FakeStorage();
    const store = new SetStore(storage, tick);
    store.save("A", grid());
    const id = store.list()[0]!.id;
    const key = `cfbsim.builder.set.${id}`;
    storage.items.set(key, storage.items.get(key)!.replace(`"version":${SET_VERSION}`, '"version":999'));
    expect(store.read(id)).toBeNull();
    storage.items.set("cfbsim.builder.sets", JSON.stringify({ version: 999, sets: [] }));
    expect(store.list()).toEqual([]);

    const none = new SetStore(null);
    expect(none.list()).toEqual([]);
    expect(none.save("A", grid()).ok).toBe(false);
  });

  it("refuses an empty name", () => {
    const store = new SetStore(new FakeStorage(), tick);
    expect(store.save("   ", grid())).toEqual({ ok: false, reason: "Give the set a name." });
  });
});
