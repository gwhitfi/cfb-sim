import { describe, expect, it } from "vitest";
import { parseTracking } from "./tracking";
import { attacksRight, followX, viewTurns } from "./view";

// A one-player file: agent 0 is on `team` (the offense), with `details`.
function rec(team: "home" | "away", details: object, ball: [number, number, number] | null = [30, 26, 0]) {
  const header = {
    type: "header",
    format_version: 1,
    tick_rate: 20,
    details,
    players: [{ id: "a", name: "A", label: "QB", team, jersey: 1, position: "QB", radius: 0.4 }],
  };
  const tick = (n: number, x: number) => ({ type: "tick", tick: n, p: [[x, 20, 0, 0, 0]], ball: ball && [ball[0] + n, ball[1], ball[2]] });
  return parseTracking([header, tick(0, 10), tick(1, 12)].map((l) => JSON.stringify(l)).join("\n"));
}

describe("view", () => {
  it("reads which way each team attacks", () => {
    const r = rec("away", { home_right: false });
    expect(attacksRight(r, "home")).toBe(false);
    expect(attacksRight(r, "away")).toBe(true);
    // An older file: only the offense's direction.
    const old = rec("away", { direction: "left" });
    expect(attacksRight(old, "away")).toBe(false);
    expect(attacksRight(old, "home")).toBe(true);
    expect(attacksRight(rec("home", {}), "home")).toBe(null);
  });

  it("puts the chosen team's attack up the screen", () => {
    const r = rec("away", { home_right: false }); // away offense, attacking +x
    expect(viewTurns(r, "horizontal", "offense")).toBe(0);
    expect(viewTurns(r, "vertical", "offense")).toBe(1); // +x up
    expect(viewTurns(r, "vertical", "away")).toBe(1);
    expect(viewTurns(r, "vertical", "home")).toBe(3); // +x down
    expect(viewTurns(rec("home", {}), "vertical", "offense")).toBe(1); // a drill: +x up
  });

  it("follows the ball, or the players without one", () => {
    expect(followX(rec("home", {}), 0.5)).toBeCloseTo(30.5);
    expect(followX(rec("home", {}, null), 0.5)).toBeCloseTo(11);
  });
});
