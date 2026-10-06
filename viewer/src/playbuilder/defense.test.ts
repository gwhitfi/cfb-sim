import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import * as D from "./defense";
import { layout, receiverLabels, type Scene } from "./draw";
import { EMPTY_LIBRARY } from "./draw";
import * as O from "./offense";
import { parseJsonText, readPasted, type Obj } from "./paste";

const data = (path: string) => parseJsonText(readFileSync(new URL(`../../../data/${path}`, import.meta.url), "utf8")) as Obj;

describe("the starting defense", () => {
  it("is 4-3 Over Cover 3, as data/ has it", () => {
    const f = D.defaultDefenseFormation();
    expect(D.formationProblems(f)).toEqual([]);
    // Compared after a read, since the file writes some offsets of 0 and the builder leaves them out.
    expect(D.formationJson(f)).toEqual(D.formationJson(D.parseDefenseFormation(data("formations/defense/43_over.json"))));
    const book = data("playbooks/43_base.json");
    expect(D.playJson(f, D.defaultDefensePlay())).toEqual((book.plays as Obj[])[0]);
  });
});

describe("defense JSON", () => {
  it("reads and writes back every play in data/playbooks/43_base.json", () => {
    const f = D.parseDefenseFormation(data("formations/defense/43_over.json"));
    for (const play of data("playbooks/43_base.json").plays as Obj[]) {
      const { play: p, warnings } = D.parseDefensePlay(play, f);
      expect(warnings).toEqual([]);
      expect(D.playJson(f, p)).toEqual(play);
    }
  });
  it("writes man leverage, align, and blitzes the way the playbooks do", () => {
    expect(D.passJson({ kind: "man", target: "#1_strong", leverage: "outside" })).toEqual({ man: { target: "#1_strong", leverage: "outside" } });
    expect(D.passJson({ kind: "man", target: "back_1", leverage: "inside" })).toEqual({ man: "back_1" });
    expect(D.passJson({ kind: "blitz", gap: "D", side: "strong" })).toEqual({ blitz: { gap: "D", side: "strong" } });
    const cover1 = {
      id: "c1", name: "C1", side: "defense", formation: "43_over", coverage: "cover_1",
      assignments: { WILL: { run: { gap: "B", side: "weak" }, pass: { man: "#2_weak" }, align: { x: { over: "#2", side: "weak", offset: -1 }, y: 5 } } },
      tags: [],
    };
    const { play } = D.parseDefensePlay(cover1, D.defaultDefenseFormation());
    expect(D.playJson({ ...D.defaultDefenseFormation(), slots: D.defaultDefenseFormation().slots.filter((s) => s.name === "WILL") }, play)).toEqual(cover1);
  });
});

describe("what each defender is offered", () => {
  it("leads with the usual job for his position", () => {
    const f = D.defaultDefenseFormation();
    const s = (n: string) => f.slots.find((x) => x.name === n)!;
    expect(D.makeAssign(s("SDE"), "strong")).toMatchObject({ run: { kind: "gap" }, pass: { kind: "rush" } });
    expect(D.makeAssign(s("MIKE"), "strong")).toMatchObject({ run: { kind: "gap" }, pass: { kind: "zone" } });
    expect(D.makeAssign(s("SCB"), "weak")).toMatchObject({ run: { kind: "support", ref: "deep_third" }, pass: { kind: "zone", ref: "deep_third_weak" } });
  });
  it("fills in a new slot and drops a removed one", () => {
    const f = D.defaultDefenseFormation();
    const p = D.defaultDefensePlay();
    f.slots = f.slots.filter((s) => s.name !== "SAM");
    f.slots.push(D.newDefSlot("CB", f.slots.map((s) => s.name)));
    D.normalizeDefense(f, p);
    expect(p.assignments.SAM).toBeUndefined();
    expect(p.assignments.NB).toBeDefined();
  });
  it("flags two defenders on one man", () => {
    const f = D.defaultDefenseFormation();
    const p = D.defaultDefensePlay();
    p.assignments.SS!.pass = { kind: "man", target: "#2_strong", leverage: "inside" };
    p.assignments.SAM!.pass = { kind: "man", target: "#2_strong", leverage: "inside" };
    expect(D.playProblems(f, p, ["te"], []).join()).toMatch(/SAM and SS both have #2_strong/);
    expect(D.playProblems(f, D.defaultDefensePlay(), [], []).join()).toMatch(/strength call/);
  });
});

describe("the defense on the field", () => {
  const sc = (): Scene => ({
    mode: "defense",
    hash: "middle",
    lib: EMPTY_LIBRARY,
    off: { formation: O.defaultOffenseFormation(), play: O.defaultOffensePlay() },
    def: { formation: D.defaultDefenseFormation(), play: D.defaultDefensePlay(), calls: ["te", "passing_strength", "field"], override: "", view: "pass" },
    selected: "",
  });
  it("lines up against the offense: strong to trips, corners over #1", () => {
    const lay = layout(sc());
    expect(lay.error).toBe("");
    expect(lay.read!.strong).toBe("right");
    expect(lay.def.SCB!.x).toBeCloseTo(19.67, 2);
    expect(lay.def.SDE!.x).toBeCloseTo(3.5);
    expect(receiverLabels(lay.read!)).toMatchObject({ Z: "#1s", H: "#2s", Y: "#3s", X: "#1w", RB: "b1" });
  });
  it("uses a play's align over the formation's spot", () => {
    const s = sc();
    s.def.play.assignments.FS!.align = { y: 6 };
    expect(layout(s).def.FS!.y).toBe(6);
  });
  it("says when the offense can't be read", () => {
    const s = sc();
    s.off.formation.slots = s.off.formation.slots.filter((x) => x.name !== "RT");
    expect(layout(s).error).toMatch(/guard and a tackle on the right/);
  });
});

describe("pasting", () => {
  it("tells formations, plays, and playbooks apart", () => {
    expect(readPasted(JSON.stringify(data("formations/defense/43_over.json")))).toMatchObject({ kind: "formation", side: "defense" });
    expect(readPasted(readFileSync(new URL("../../../data/playbooks/43_base.json", import.meta.url), "utf8"))).toMatchObject({ kind: "playbook", side: "defense", strengthCall: ["te", "passing_strength", "field"] });
    expect(readPasted(JSON.stringify((data("playbooks/spread_base.json").plays as Obj[])[0]))).toMatchObject({ kind: "play", side: "offense" });
  });
  it("explains what it can't read", () => {
    expect(() => readPasted("{ nope")).toThrow(/not valid JSON/);
    expect(() => readPasted('{"side": "special", "plays": []}')).toThrow(/special teams/);
    expect(() => readPasted('{"side": "offense"}')).toThrow(/couldn't tell/);
  });
});
