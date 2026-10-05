import { describe, expect, it } from "vitest";
import { answersTo, kickYardLine, playRequest, playTitle, stepSeed, type PlayForm, type TeamInfo } from "./plays";

const form: PlayForm = {
  offense: "r80_a",
  play: "gun_inside_zone",
  defense: "r80_b",
  defPlay: "43_over_cover3",
  seed: 7,
  hash: "left",
  yardLine: 30,
  flip: true,
  huddle: false,
};

const teams: TeamInfo[] = [
  { id: "r80_a", name: "Test 80 A", abbr: "R80A", offense: [{ id: "gun_inside_zone", name: "Gun Inside Zone", type: "run", formation: "gun_2x2" }] },
  { id: "r80_b", name: "Test 80 B", abbr: "R80B", defense: [{ id: "43_over_cover3", name: "Over Cover 3", type: "defense", formation: "43_over" }] },
];

describe("run a play", () => {
  it("builds the server's run_play message", () => {
    expect(playRequest(form)).toEqual({
      type: "run_play",
      offense: "r80_a",
      play: "gun_inside_zone",
      defense: "r80_b",
      def_play: "43_over_cover3",
      seed: 7,
      hash: "left",
      yard_line: 30,
      flip: true,
      huddle: false,
    });
  });

  it("leaves the defense out for an empty field", () => {
    const msg = playRequest({ ...form, defense: "", defPlay: "" });
    expect(msg).not.toHaveProperty("defense");
    expect(msg).not.toHaveProperty("def_play");
  });

  it("steps seeds, never below 1", () => {
    expect(stepSeed(7, 1)).toBe(8);
    expect(stepSeed(1, -1)).toBe(1);
    expect(stepSeed(Number.NaN, 0)).toBe(1);
    expect(stepSeed(4.6, 0)).toBe(5);
  });

  it("names the play by teams, plays, and seed", () => {
    expect(playTitle(form, teams)).toBe("R80A Gun Inside Zone vs R80B Over Cover 3 · seed 7");
    expect(playTitle({ ...form, defense: "", defPlay: "" }, teams)).toBe("R80A Gun Inside Zone vs empty field · seed 7");
  });
});

describe("run a kick", () => {
  const kick: PlayForm = {
    mode: "kicks",
    offense: "r80_a",
    play: "kickoff_deep",
    defense: "r80_b",
    defPlay: "kr_middle",
    seed: 4,
    hash: "middle",
    yardLine: 35,
    flip: false,
    huddle: false,
  };
  const st: TeamInfo[] = [
    { id: "r80_a", name: "Test 80 A", abbr: "R80A", kicking: [{ id: "kickoff_deep", name: "Kickoff: Deep Middle", type: "kickoff", formation: "kickoff_std" }] },
    { id: "r80_b", name: "Test 80 B", abbr: "R80B", receiving: [{ id: "kr_middle", name: "Kickoff Return: Middle", type: "kickoff_return", formation: "kickoff_return_std" }] },
  ];

  it("builds the server's run_kick message", () => {
    expect(playRequest(kick)).toEqual({
      type: "run_kick",
      kicking: "r80_a",
      kick: "kickoff_deep",
      receiving: "r80_b",
      return: "kr_middle",
      seed: 4,
      hash: "middle",
      yard_line: 35,
    });
  });

  it("offers only the returns for the kind of kick", () => {
    const plays = [
      { id: "kr_middle", name: "", type: "kickoff_return", formation: "" },
      { id: "pr_middle", name: "", type: "punt_return", formation: "" },
      { id: "fg_block", name: "", type: "field_goal_block", formation: "" },
    ];
    expect(answersTo("kickoff", plays).map((p) => p.id)).toEqual(["kr_middle"]);
    expect(answersTo("punt", plays).map((p) => p.id)).toEqual(["pr_middle"]);
    expect(answersTo("field_goal", plays).map((p) => p.id)).toEqual(["fg_block"]);
  });

  it("kicks from each kind's usual spot", () => {
    expect(kickYardLine("kickoff")).toBe(35);
    expect(kickYardLine("punt")).toBe(30);
    expect(kickYardLine("field_goal")).toBe(70);
  });

  it("names the kick by teams, kick, return, and seed", () => {
    expect(playTitle(kick, st)).toBe("R80A Kickoff: Deep Middle vs R80B Kickoff Return: Middle · seed 4");
  });
});
