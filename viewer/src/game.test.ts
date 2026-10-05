import { describe, expect, it } from "vitest";
import { byFormation, clockText, describeLine, downText, gameClockLeft, periodText, playClockAt, playClockLeft, StepQueue, type GameState } from "./game";

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
      check_from: "nickel_cover1",
      fouls: [{ foul: "offensive_holding", team: "home", flagged: true }] };
    const text = describeLine(play, state)!;
    expect(text).toContain("STU 3rd & 6 at TECH 40: Complete to X");
    expect(text).toContain("audible from smash");
    expect(text).toContain("defense checked from nickel_cover1");
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

  it("joins the parts of a streamed file still in the queue", () => {
    const q = new StepQueue();
    const part = (data: string, partial: boolean) => ({ type: "tracking" as const, name: "007_0_between", data, partial });
    q.push({ messages: [part("a", true)], prompt: null, done: false });
    q.push({ messages: [part("b", true), part("c", false)], prompt: null, done: false });
    expect(q.length).toBe(1);
    expect(q.shift()).toEqual({ kind: "tracking", name: "007_0_between", data: "abc", streaming: false });
  });

  it("marks only a streamed file as streamed (a walk-on comes whole)", () => {
    const q = new StepQueue();
    q.push({ messages: [{ type: "tracking", name: "001_0_between", data: "x" }], prompt: null, done: false });
    expect(q.shift()).toEqual({ kind: "tracking", name: "001_0_between", data: "x" });
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

describe("playClockLeft", () => {
  it("runs from the file's start", () => {
    expect(playClockLeft(40, 12.5, false)).toBe(27.5);
  });
  it("waits for the ready signal after a change of possession", () => {
    expect(playClockLeft(25, 6, true)).toBe(25); // not ready yet
    expect(playClockLeft(25, 6, true, 8.4)).toBe(25); // ready later in the file
    expect(playClockLeft(25, 18.4, true, 8.4)).toBeCloseTo(15);
  });
});

describe("playClockAt", () => {
  it("finds when the play clock reads a value", () => {
    expect(playClockAt(40, 12, false)).toBe(28);
    expect(playClockAt(25, 12, true)).toBeUndefined();
    expect(playClockAt(25, 12, true, 8)).toBe(21);
  });
});

describe("gameClockLeft", () => {
  it("runs with the play clock when the clock is running", () => {
    expect(gameClockLeft({ game_clock: 600, game_clock_starts: 0 }, 12)).toBe(588);
  });
  it("starts on the ready signal", () => {
    expect(gameClockLeft({ game_clock: 600, game_clock_starts: 7 }, 5)).toBe(600);
    expect(gameClockLeft({ game_clock: 600, game_clock_starts: 7 }, 12)).toBe(595);
    // A 25-second clock: from the ready event in the file.
    const admin = { game_clock: 600, game_clock_starts: 0, waits_for_ready: true };
    expect(gameClockLeft(admin, 6)).toBe(600);
    expect(gameClockLeft(admin, 10, 8)).toBe(598);
  });
  it("stays stopped until the snap, and stops at the two-minute timeout", () => {
    expect(gameClockLeft({ game_clock: 600 }, 30)).toBe(600);
    expect(gameClockLeft({ game_clock: 125, game_clock_starts: 0, game_clock_stops: 120 }, 20)).toBe(120);
    expect(gameClockLeft({ game_clock: 10, game_clock_starts: 0 }, 20)).toBe(0);
  });
  it("is null with no game clock (overtime)", () => {
    expect(gameClockLeft({}, 5)).toBeNull();
  });
  it("formats like the scoreboard", () => {
    expect(clockText(754)).toBe("12:34");
    expect(clockText(59.2)).toBe("1:00");
    expect(clockText(0)).toBe("0:00");
  });
});
