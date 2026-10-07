import { describe, expect, it } from "vitest";
import type { DefensivePlaybookArt, FormationArt, PlayArt, PlaybookArt } from "./art";
import type { PlayOption } from "./game";
import { menuGroups, missingArt, MenuPage, menuPageSize } from "./playmenu";

const formation = (id: string): FormationArt => ({ id, name: id, personnel: "11", players: [] });
const play = (id: string, f: string): PlayArt => ({ id, name: id, formation: f, type: "pass", flip: true, routes: [], qb: [], path: null, pulls: [] });
const option = (id: string, f: string): PlayOption => ({ id, name: id, formation: f, type: "pass", flip: true, tags: [] });

const art: PlaybookArt = {
  team: "t",
  playbook: "b",
  formations: [formation("trips"), formation("doubles"), formation("empty")],
  plays: [play("stick", "trips"), play("smash", "doubles"), play("curl", "trips"), play("quads", "empty")],
};

describe("the play menu's formations and plays", () => {
  it("before the call opens: every formation and play, in the playbook's order", () => {
    const g = menuGroups(art, null);
    expect(g.map((x) => x.formation.id)).toEqual(["trips", "doubles", "empty"]);
    expect(g[0]!.plays.map((p) => p.id)).toEqual(["stick", "curl"]);
  });

  it("once it's open: only the call's plays, and no formation left empty", () => {
    const g = menuGroups(art, [option("curl", "trips"), option("smash", "doubles")]);
    expect(g.map((x) => x.formation.id)).toEqual(["trips", "doubles"]);
    expect(g[0]!.plays.map((p) => p.id)).toEqual(["curl"]);
  });

  it("names the call's plays that have no picture", () => {
    expect(missingArt(art, [option("curl", "trips"), option("new_play", "trips")]).map((p) => p.id)).toEqual(["new_play"]);
    expect(missingArt(art, null)).toEqual([]);
  });
});

const defense: DefensivePlaybookArt = {
  unit: "defense", team: "t", playbook: "d",
  formations: ["base", "nickel"].map((id) => ({ id, name: id, package: id, players: [] })),
  plays: [["sky", "base"], ["fire", "base"], ["quarters", "nickel"], ["orphan", "missing"]]
    .map(([id, formation]) => ({ id: id!, name: id!, formation: formation!, coverage: "cover_3", players: [], jobs: [] })),
};

describe("defensive legal choices", () => {
  it("keeps book order and hides unoffered plays and empty packages", () => {
    const groups = menuGroups(defense, [option("fire", "base"), option("sky", "base")]);
    expect(groups.map((g) => [g.unit, g.formation.id, g.plays.map((p) => p.id)]))
      .toEqual([["defense", "base", ["sky", "fire"]]]);
    expect(menuGroups(defense, [])).toEqual([]);
  });
  it("keeps missing plays and orphaned art available as text", () => {
    expect(missingArt(defense, [option("sky", "base"), option("orphan", "missing"), option("new", "base")])
      .map((p) => p.id)).toEqual(["orphan", "new"]);
  });
});


describe("paged card navigation", () => {
  it("uses exactly three, two, or one slots at the picker width boundaries", () => {
    expect([1500, 840, 839, 560, 559, 280].map(menuPageSize)).toEqual([3, 3, 2, 2, 1, 1]);
  });
  it("handles empty, single, partial and cycling pages", () => {
    const p = new MenuPage();
    p.sync([], 3); p.cycle(1); p.move(-1);
    expect([p.count, p.page, p.visible, p.focused]).toEqual([0, 0, [], null]);
    p.sync(["a"], 3); p.cycle(-1);
    expect(p.visible).toEqual(["a"]);
    p.sync(["a", "b", "c", "d", "e", "f", "g"], 3);
    p.cycle(-1); expect(p.visible).toEqual(["g"]);
    p.cycle(1); expect(p.visible).toEqual(["a", "b", "c"]);
    p.cycle(1); expect(p.visible).toEqual(["d", "e", "f"]);
  });
  it("moves focus through page boundaries and wraps both ways", () => {
    const p = new MenuPage(); p.sync(["a", "b", "c", "d"], 3);
    p.reveal("c"); p.move(1);
    expect([p.page, p.focused]).toEqual([1, "d"]);
    p.move(1); expect([p.page, p.focused]).toEqual([0, "a"]);
    p.move(-1); expect([p.page, p.focused]).toEqual([1, "d"]);
  });
  it("retains the first visible item on resize and clamps a removed page", () => {
    const p = new MenuPage(); const ids = ["a", "b", "c", "d", "e", "f", "g"];
    p.sync(ids, 3); p.cycle(1); p.sync(ids, 2);
    expect(p.visible).toContain("d");
    p.sync(ids, 1); expect(p.visible).toEqual(["c"]);
    p.reveal("g"); p.sync(["a", "b"], 1);
    expect([p.page, p.focused, p.visible]).toEqual([1, "b", ["b"]]);
  });
  it("preserves a surviving focus and anchor when legal options change", () => {
    const p = new MenuPage(); p.sync(["a", "b", "c", "d", "e", "f"], 3);
    p.reveal("e"); p.sync(["a", "b", "c", "d", "e"], 3);
    expect([p.page, p.focused]).toEqual([1, "e"]);
    p.reveal("unknown"); expect(p.focused).toBe("e");
  });
});
