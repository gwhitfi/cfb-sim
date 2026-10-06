import { describe, expect, it } from "vitest";
import type { PlayInfo } from "../plays";
import { DEFAULT_KICK_FORM, answers, applyingCells, groupByType, kickProblem, kickRequest, parseKickForm, planKickRuns } from "./kickmatchup";

const play = (id: string, type: string): PlayInfo => ({ id, name: id, type, formation: type });
const kickoff = play("kickoff_deep", "kickoff"), squib = play("kickoff_squib", "kickoff"), punt = play("punt_normal", "punt"), fg = play("fg_kick", "field_goal");
const kr = play("kr_middle", "kickoff_return"), pr = play("pr_middle", "punt_return"), block = play("fg_block", "field_goal_block");

describe("which cells run", () => {
  it("a return answers only its own kind of kick", () => {
    expect(answers(kickoff, kr)).toBe(true);
    expect(answers(punt, kr)).toBe(false);
    expect(answers(fg, block)).toBe(true);
    expect(answers(fg, pr)).toBe(false);
  });
  it("counts the pairs that run", () => {
    expect(applyingCells([kickoff, squib, punt, fg], [kr, pr, block])).toBe(4); // 2 kickoffs + punt + fg
    expect(applyingCells([kickoff, squib], [pr, block])).toBe(0);
  });
});

describe("planKickRuns", () => {
  it("per cell: runs times the cells that run, not the whole grid", () => {
    expect(planKickRuns([kickoff, punt], [kr, pr, block], "per_cell", 50)).toEqual({ cells: 2, perCell: 50, total: 100 });
  });
  it("total: split over the cells that run", () => {
    expect(planKickRuns([kickoff, squib, punt, fg], [kr, pr, block], "total", 1000)).toEqual({ cells: 4, perCell: 250, total: 1000 });
  });
});

describe("the server message", () => {
  const f = { ...DEFAULT_KICK_FORM, kicking: "a", receiving: "b", kicks: ["punt_normal"], returns: ["pr_middle"], seed: 4, aggression: 70 };
  it("sends the kind, the explicit lists, and runs per cell", () => {
    expect(kickRequest(f, { cells: 1, perCell: 25, total: 25 })).toEqual({
      type: "run_matrix", kind: "kicks", kicking: "a", receiving: "b", kicks: ["punt_normal"], returns: ["pr_middle"],
      runs: 25, seed: 4, return_aggression: 70,
    });
  });
  it("sends the hash and yard line only when set", () => {
    const m = kickRequest({ ...f, hash: "left", yardLine: 40 }, { cells: 1, perCell: 1, total: 1 });
    expect(m).toMatchObject({ hash: "left", yard_line: 40 });
  });
});

describe("kickProblem", () => {
  const ok = { ...DEFAULT_KICK_FORM, kicking: "a", receiving: "b" };
  it("accepts kicks with an answer", () => {
    expect(kickProblem(ok, [punt], [pr])).toBeNull();
  });
  it("names what's missing", () => {
    expect(kickProblem({ ...ok, receiving: "" }, [punt], [pr])).toMatch(/teams/);
    expect(kickProblem(ok, [], [pr])).toMatch(/kick/);
    expect(kickProblem(ok, [punt], [])).toMatch(/return/);
    expect(kickProblem(ok, [punt], [kr, block])).toMatch(/None of the chosen/);
  });
});

describe("groups and saved forms", () => {
  it("groups by kind with names", () => {
    expect(groupByType([kickoff, squib, punt]).map((g) => [g.name, g.plays.length])).toEqual([["Kickoffs", 2], ["Punts", 1]]);
    expect(groupByType([kr, block]).map((g) => g.name)).toEqual(["Kickoff returns", "Field goal blocks"]);
  });
  it("parses a saved form with defaults for the wrong parts", () => {
    expect(parseKickForm(null)).toEqual(DEFAULT_KICK_FORM);
    expect(parseKickForm({ aggression: 900, yardLine: "x", mode: "no", kicks: [1, "punt_normal"] })).toEqual({
      ...DEFAULT_KICK_FORM, aggression: 100, kicks: ["punt_normal"],
    });
    const f = { ...DEFAULT_KICK_FORM, kicking: "a", yardLine: 40, hash: "left" as const };
    expect(parseKickForm(JSON.parse(JSON.stringify(f)))).toEqual(f);
  });
});
