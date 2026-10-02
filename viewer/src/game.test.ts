import { describe, expect, it } from "vitest";
import { byFormation, describeLine, downText, periodText, readyReading, StepQueue, type GameState } from "./game";

const state: GameState = {
  tick: 0,
  quarter: 2,
  overtime: false,
  clock: 754,
  clock_text: "12:34",
  clock_running: true,
  play_clock: 40,
  teams: { home: { id: "state_u", name: "State", abbr: "STU" }, away: { id: "tech", name: "Tech", abbr: "TECH" } },
  score: { home: 7, away: 3 },
  timeouts: { home: 3, away: 2 },
  offense: "home",
  down: 3,
  distance: 6.4,
  goal_to_go: false,
  yard_line: 60,
  situation: "3rd & 6 at TECH 40",
  hash: "middle",
  offense_right: true,
};

describe("game text", () => {
  it("formats the scoreboard pieces", () => {
    expect(periodText(state)).toBe("2nd");
    expect(downText(state)).toBe("3rd & 6");
    expect(downText({ ...state, goal_to_go: true })).toBe("3rd & Goal");
    expect(periodText({ ...state, quarter: 6, overtime: true })).toBe("OT2");
  });

  it("describes plays and events", () => {
    const play = { type: "play", offense: "home", situation: "3rd & 6 at TECH 40", summary: "Complete to X", audible_from: "smash",
      fouls: [{ foul: "offensive_holding", team: "home", flagged: true }] };
    const text = describeLine(play, state)!;
    expect(text).toContain("STU 3rd & 6 at TECH 40: Complete to X");
    expect(text).toContain("audible from smash");
    expect(text).toContain("FLAG: Holding (offense) STU (declined)");
    expect(describeLine({ type: "event", event: "timeout", team: "away", left: 1, clock: "1:02" }, state)).toBe(
      "Timeout TECH (1 left) at 1:02",
    );
    expect(describeLine({ type: "event", event: "sub_out", team: "home", player: "stu_1", energy: 0.6 }, state,
      new Map([["stu_1", "J. Smith"]]))).toBe("STU sub: J. Smith out (60% energy)");
    expect(describeLine({ type: "input" }, state)).toBeNull();
  });
});

describe("prompts", () => {
  it("groups plays by formation in order", () => {
    const plays = [
      { id: "a", name: "A", formation: "f1", type: "pass", flip: true, tags: [] },
      { id: "b", name: "B", formation: "f2", type: "run", flip: true, tags: [] },
      { id: "c", name: "C", formation: "f1", type: "run", flip: false, tags: [] },
    ];
    expect(byFormation(plays).map(([f, ps]) => [f, ps.map((p) => p.id)])).toEqual([["f1", ["a", "c"]], ["f2", ["b"]]]);
  });

  it("starts the play clock at the tempo's ready reading", () => {
    const o = { play_clock: 40, ready: { huddle: 16, no_huddle: 22, hurry: 33 } };
    expect(readyReading(o, "hurry")).toBe(33);
    expect(readyReading(o, "nonsense")).toBe(16);
  });
});

describe("StepQueue", () => {
  it("keeps snaps and lines in order", () => {
    const q = new StepQueue();
    q.push({
      messages: [
        { type: "tracking", name: "001", data: "x" },
        { type: "line", line: { type: "play" }, state },
      ],
      prompt: null,
      done: false,
    });
    expect(q.length).toBe(2);
    expect(q.shift()!.kind).toBe("tracking");
    expect(q.shift()!.kind).toBe("line");
    expect(q.empty).toBe(true);
  });
});
