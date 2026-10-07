import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import * as O from "./offense";
import { parseJsonText, type Obj } from "./paste";

const data = (path: string) => parseJsonText(readFileSync(new URL(`../../../data/${path}`, import.meta.url), "utf8")) as Obj;
const routes = data("library/routes.json");
const qb = data("library/qb_actions.json");
const blocking = data("library/blocking_schemes.json");
const LIB: O.LibNames = {
  routes: Object.keys(routes.routes as Obj), drops: Object.keys(qb.drops as Obj), fakes: Object.keys(qb.fakes as Obj),
  protections: Object.keys(blocking.protections as Obj), runSchemes: Object.keys(blocking.run_schemes as Obj),
};

// A play with every slot given the usual job for its position.
function fresh(type: O.PlayType = "pass") {
  const f = O.defaultOffenseFormation();
  const p = O.defaultOffensePlay();
  p.type = type;
  O.normalizeOffense(f, p);
  return { f, p };
}
const slot = (f: O.OffFormation, name: string) => f.slots.find((s) => s.name === name)!;

describe("the starting formation", () => {
  it("is Gun Wide Trips, as data/ has it", () => {
    const f = O.defaultOffenseFormation();
    expect(O.formationProblems(f)).toEqual([]);
    expect(O.formationJson(f)).toEqual(data("formations/offense/gun_wide_trips.json"));
  });
  it("reports what the loader rejects", () => {
    const f = O.defaultOffenseFormation();
    expect(O.formationProblems({ ...f, slots: f.slots.slice(1) }).join()).toMatch(/10 players/);
    expect(O.formationProblems({ ...f, slots: f.slots.map((s) => ({ ...s, onLine: false })) }).join()).toMatch(/0 on the line/);
    expect(O.formationProblems({ ...f, slots: f.slots.map((s) => ({ ...s, stance: s.stance === "snap" ? "three_point" : s.stance })) }).join()).toMatch(/0 slots with stance 'snap'/);
    expect(O.formationProblems({ ...f, slots: f.slots.map((s) => (s.name === "RB" ? { ...s, name: "QB" } : s)) }).join()).toMatch(/two slots are named QB/);
  });
});

describe("what each position is offered", () => {
  it("depends on the position and the play type", () => {
    const { f, p } = fresh();
    expect(O.choicesFor(slot(f, "LT"), p)).toEqual(["protect", "block"]);
    expect(O.choicesFor(slot(f, "QB"), p)).toEqual(["none"]);
    expect(O.choicesFor(slot(f, "RB"), p)[0]).toBe("release");
    expect(O.choicesFor(slot(f, "X"), p)).not.toContain("protect");
    p.type = "run";
    expect(O.choicesFor(slot(f, "LT"), p)).toEqual(["scheme", "block"]);
    expect(O.choicesFor(slot(f, "RB"), p)).toEqual(["none"]); // the carrier
  });
  it("gives the usual jobs: protect and routes on a pass, scheme and stalk on a run", () => {
    const { f, p } = fresh();
    const map = O.currentAssignments(p);
    expect(O.choiceOf(map.LT!)).toBe("protect");
    expect(O.choiceOf(map.X!)).toBe("route");
    expect(O.choiceOf(map.RB!)).toBe("release");
    expect(map.QB).toEqual({ kind: "none" });
    p.type = "run";
    O.normalizeOffense(f, p);
    const run = O.currentAssignments(p);
    expect(O.choiceOf(run.LT!)).toBe("scheme");
    expect(run.X).toEqual({ kind: "block", block: { action: "stalk", target: "man_over" } });
    expect(run.RB).toEqual({ kind: "none" });
    expect(O.playProblems(f, p, LIB)).toEqual([]);
  });
  it("keeps each play type's assignments when switching back", () => {
    const { f, p } = fresh();
    O.currentAssignments(p).X = { kind: "route", route: { route: "slant" } };
    p.type = "run";
    O.normalizeOffense(f, p);
    p.type = "pass";
    O.normalizeOffense(f, p);
    expect(O.currentAssignments(p).X).toEqual({ kind: "route", route: { route: "slant" } });
  });
  it("replaces what the loader rejects: protect on a run, scheme on a pass", () => {
    const { f, p } = fresh("run");
    O.currentAssignments(p).LT = { kind: "block", block: { action: "protect", target: "" } };
    expect(O.playProblems(f, p, LIB).join()).toMatch(/LT: 'protect' isn't allowed on a run play/);
    O.normalizeOffense(f, p);
    expect(O.choiceOf(O.currentAssignments(p).LT!)).toBe("scheme");
  });
});

describe("the progression", () => {
  it("is checked like the loader checks it", () => {
    const { f, p } = fresh();
    expect(O.playProblems(f, p, LIB).join()).toMatch(/progression is empty/);
    p.pass.progression = ["Z", "H"];
    p.pass.hot = "RB";
    expect(O.playProblems(f, p, LIB)).toEqual([]);
  });
  it("drops a slot that stops running a route", () => {
    const { f, p } = fresh();
    p.pass.progression = ["Z", "H"];
    p.pass.hot = "H";
    O.currentAssignments(p).H = { kind: "block", block: { action: "pass_set", target: "" } };
    O.normalizeOffense(f, p);
    expect(p.pass.progression).toEqual(["Z"]);
    expect(p.pass.hot).toBe("");
  });
  it("flags a run whose free_after names no path point", () => {
    const { f, p } = fresh("run");
    p.run.freeAfter = "cut";
    expect(O.playProblems(f, p, LIB).join()).toMatch(/free after 'cut'/);
  });
});

describe("JSON", () => {
  it("writes assignments the way the playbooks do", () => {
    expect(O.assignmentJson({ kind: "block", block: { action: "check_release", target: "" }, then: { route: "flat", side: "left" } })).toEqual({ block: { do: "check_release" }, then: { route: "flat", side: "left" } });
    expect(O.assignmentJson({ kind: "route", route: { route: "stick", vsMan: { route: "out", depth: 6 } } })).toEqual({ route: "stick", option: { vs_man: { route: "out", depth: 6 } } });
    expect(O.assignmentJson({ kind: "none" })).toBeUndefined();
  });
  it("reads and writes back every play in data/playbooks/spread_base.json", () => {
    const f = O.parseOffenseFormation(data("formations/offense/gun_wide_trips.json"));
    for (const play of (data("playbooks/spread_base.json").plays as Obj[])) {
      const { play: p, warnings } = O.parseOffensePlay(play, f);
      expect(warnings).toEqual([]);
      O.normalizeOffense(f, p);
      expect(O.playJson(f, p)).toEqual(play);
      expect(O.playProblems(f, p, LIB)).toEqual([]);
    }
  });
  it("reads a run play with a gap path and a pulling guard", () => {
    const f = O.defaultOffenseFormation();
    const power = {
      id: "gun_power", name: "Gun Power", side: "offense", type: "run", formation: "gun_wide_trips", flip: true,
      run: { scheme: "power", direction: "right", carrier: "RB", qb_action: { do: "handoff", then: "drop_fake" },
        path: [{ x: 0.3, y: -4.6, type: "mesh" }, { x: { from: "gap_C", side: "right" }, y: -1, type: "aim" }], free_after: "aim", reads: ["bang", "bend"] },
      assignments: {
        LT: { block: { do: "cutoff", target: "backside_gap" } }, LG: { block: { do: "pull_lead", target: "first_lb_playside" } },
        C: { block: { do: "scheme" } }, RG: { block: { do: "scheme" } }, RT: { block: { do: "drive", target: "emol_playside" } },
        X: { block: { do: "stalk", target: "man_over" } }, H: { block: { do: "stalk", target: "man_over" } },
        Y: { block: { do: "stalk", target: "man_over" } }, Z: { block: { do: "stalk", target: "man_over" } },
      },
      tags: ["base", "short_yardage"],
    };
    const { play: p, warnings } = O.parseOffensePlay(power, f);
    expect(warnings).toEqual([]);
    O.normalizeOffense(f, p);
    expect(O.playJson(f, p)).toEqual(power);
    expect(O.playProblems(f, p, LIB)).toEqual([]);
  });
  it("warns when a play doesn't fit the formation", () => {
    const f = O.defaultOffenseFormation();
    const { warnings } = O.parseOffensePlay({ id: "x", name: "X", side: "offense", type: "pass", formation: "gun_2x2", pass: { drop: "gun_3", progression: ["W"], protection: { scheme: "slide" } }, assignments: { W: { route: "go" } } }, f);
    expect(warnings.join()).toMatch(/formation 'gun_2x2'/);
    expect(warnings.join()).toMatch(/no slot W/);
  });
});
