import { describe, expect, it } from "vitest";
import { playRequest, playTitle, stepSeed, type PlayForm, type TeamInfo } from "./plays";

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
