import { describe, expect, it } from "vitest";
import type { GameState } from "./game";
import { lineScore, scoring, teamPlayers, teamRows, type Box, type BoxPlayer, type BoxTeam } from "./stats";

const team = (id: string, over: Partial<BoxTeam> = {}): BoxTeam => ({
  team: id, points: 0, plays: 10, total_yards: 120.4, first_downs: 6, first_downs_rush: 3, first_downs_pass: 2,
  first_downs_penalty: 1, rush_att: 5, rush_yds: 40.2, pass_att: 5, pass_cmp: 3, pass_yds: 80.2, sacks_taken: 1,
  interceptions: 1, fumbles_lost: 1, third_down: [2, 4], fourth_down: [0, 1], penalties: 2, penalty_yds: 15,
  punts: 2, punt_yds: 85, kick_returns: [1, 22], punt_returns: [0, 0], field_goals: [1, 2], possession: "7:30", ...over,
});

const state = (quarter: number, home: number, away: number): GameState => ({
  tick: 0, quarter, overtime: false, clock: 300, clock_text: "5:00", clock_running: false, play_clock: 40,
  teams: { home: { id: "h", name: "Home U", abbr: "HOM" }, away: { id: "a", name: "Away U", abbr: "AWY" } },
  score: { home, away }, timeouts: { home: 3, away: 3 }, offense: "home", down: 1, distance: 10,
  goal_to_go: false, yard_line: 25, situation: "", hash: "middle", offense_right: true,
});

const box = (players: BoxPlayer[], periods: [number, number][] = []): Box => ({ teams: [team("h"), team("a")], periods, players });

describe("stats", () => {
  it("fills the quarter being played and leaves the rest blank", () => {
    const ls = lineScore(box([], [[7, 3]]), state(2, 17, 3));
    expect(ls.heads).toEqual(["1", "2", "3", "4"]);
    expect(ls.rows.map((r) => r.key)).toEqual(["away", "home"]);
    expect(ls.rows[1]!.cells).toEqual(["7", "10", "", ""]);
    expect(ls.rows[0]!.cells).toEqual(["3", "0", "", ""]);
    expect(ls.rows[1]!.total).toBe(17);
    const ot = lineScore(box([], [[7, 7], [0, 0], [7, 7], [0, 0], [7, 0]]), state(5, 21, 14), true);
    expect(ot.heads).toEqual(["1", "2", "3", "4", "OT"]);
  });

  it("names the scorer from the box score's change", () => {
    const qb = (td: number): BoxPlayer => ({ id: "qb", name: "C. Klubnik", team: "h", passing: { cmp: 3, att: 5, yds: 80, td, int: 0 } });
    const wr = (td: number): BoxPlayer => ({ id: "wr", name: "T. Moore", team: "h", receiving: { rec: 3, targets: 4, yds: 80, td, long: 27 } });
    const k = (xp: number): BoxPlayer => ({ id: "k", name: "N. Kicker", team: "h", kicking: { fg: [0, 0], long: 0, xp: [xp, xp] } });
    const before = box([qb(0), wr(0), k(0)]);
    const after = box([qb(1), wr(1), k(0)]);
    const td = { type: "play", quarter: 1, clock: "11:52", offense: "home", yards: 27.4, outcome: "touchdown", home: 6, away: 0 };
    let list = scoring([], td, state(1, 6, 0), before, after);
    expect(list).toEqual([{ quarter: 1, clock: "11:52", team: "home", text: "T. Moore 27 yd pass from C. Klubnik", home: 6, away: 0 }]);
    const xp = { type: "event", event: "extra_point", team: "home", good: true, home: 7, away: 0 };
    list = scoring(list, xp, state(1, 7, 0), after, box([qb(1), wr(1), k(1)]));
    expect(list[0]!.text).toBe("T. Moore 27 yd pass from C. Klubnik (N. Kicker kick)");
    expect(list[0]!.home).toBe(7);
    const safety = { type: "play", quarter: 2, clock: "7:57", offense: "away", outcome: "incomplete", summary: "Incomplete", home: 9, away: 0 };
    expect(scoring(list, safety, state(2, 9, 0), after, after).at(-1)!.text).toBe("Safety");
    // A play that doesn't score adds nothing.
    expect(scoring(list, { type: "play", outcome: "tackled" }, state(1, 7, 0), after, after)).toBe(list);
  });

  it("lays out the team stats away first, and each team's leaders", () => {
    const rows = teamRows({ teams: [team("h", { total_yards: 300.4 }), team("a")], periods: [], players: [] });
    const total = rows.find((r) => r.label === "Total yards")!;
    expect([total.away, total.home]).toEqual(["120", "300"]);
    const players: BoxPlayer[] = [
      { id: "1", name: "A. One", team: "a", defense: { tackles: 7, sacks: 1, int: 0, fum_rec: 0 } },
      { id: "2", name: "B. Two", team: "a", defense: { tackles: 5, sacks: 0, int: 1, fum_rec: 0 } },
      { id: "3", name: "C. Three", team: "a", defense: { tackles: 2, sacks: 0, int: 0, fum_rec: 0 } },
      { id: "4", name: "D. Four", team: "a", defense: { tackles: 1, sacks: 0, int: 0, fum_rec: 0 } },
      { id: "5", name: "E. Five", team: "h", rushing: { att: 10, yds: 55, td: 1, long: 20 } },
    ];
    const away = teamPlayers(box(players), 1);
    expect(away.tables).toEqual([]);
    expect(away.defense).toEqual([
      { label: "Tackles", text: "A. One 7 · B. Two 5 · C. Three 2" },
      { label: "Sacks", text: "A. One 1" },
      { label: "Interceptions", text: "B. Two 1" },
    ]);
    const home = teamPlayers(box(players), 0);
    expect(home.tables[0]!.rows).toEqual([["E. Five", "10", "55", "5.5", "1", "20"]]);
  });
});
