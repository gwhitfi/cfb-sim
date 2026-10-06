import { describe, expect, it } from "vitest";
import type { FormationArt, PlayArt, PlaybookArt } from "./art";
import type { PlayOption } from "./game";
import { menuGroups, missingArt } from "./playmenu";

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
