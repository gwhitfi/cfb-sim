import { describe, expect, it } from "vitest";
import { buildCommentary, forYards, gain, lastName, lineAt, spotName, teamColors } from "./commentary";
import { parseTracking } from "./tracking";

const player = (label: string, name: string, team: "home" | "away") => ({
  id: label, name, label, team, jersey: 1, position: label, radius: 0.45,
});

// Home has the ball going right from its own 25 (x = 35); X stands at `xAt[tick]`.
function play(events: object[], xAt: number[] = [30, 33, 36, 50, 60]): ReturnType<typeof parseTracking> {
  const header = {
    type: "header", format_version: 2, sim_version: "0.1.0", kind: "play", name: "test", seed: 1, tick_rate: 20,
    teams: { home: { name: "Clemson", abbr: "CLEM", color: "#F56600" }, away: { name: "Rival", abbr: "RIV", color: "#522D80" } },
    details: { team: "clemson", direction: "right", home_right: true, yard_line: 25 },
    players: [
      player("QB", "C. Klubnik", "home"), player("X", "T. Moore", "home"), player("RB", "A. Randall", "home"),
      player("WCB", "J. Lukus", "away"), player("FS", "K. Barnes", "away"), player("WDE", "W. Heldt", "away"),
    ],
  };
  const ticks = xAt.map((x, t) => ({
    type: "tick", tick: t, ball: null,
    p: [[30, 26, 0, 0, 0], [x, 10, 0, 0, 0], [28, 26, 0, 0, 0], [x + 1, 10, 0, 0, 0], [x + 2, 12, 0, 0, 0], [36, 30, 0, 0, 0]],
  }));
  const lines = [header, ...ticks, ...events.map((e) => ({ type: "event", ...e }))];
  return parseTracking(lines.map((l) => JSON.stringify(l)).join("\n"));
}

const texts = (rec: ReturnType<typeof parseTracking>) => buildCommentary(rec).map((l) => l.text);

describe("commentary", () => {
  it("calls a completion with names, the spot, the tackle, and the summary", () => {
    const rec = play([
      { tick: 0, event: "snap" },
      { tick: 1, event: "throw", from: "QB", to: "X", air_yards: 8, openness: 0.45 },
      { tick: 2, event: "catch", player: "X", air_yards: 8 },
      { tick: 3, event: "drag_down", player: "WCB", on: "X" },
      { tick: 4, event: "tackle", player: "FS", on: "X" },
      { tick: 4, event: "tackled", yards: 27.2 },
    ]);
    expect(texts(rec)).toEqual([
      "Klubnik takes the snap.",
      "Klubnik throws it, he's targeting Moore... He's open!",
      "Moore catches it at his own 26!",
      "Lukus is on him, he drags him down!",
      "Barnes comes in to finish the tackle.",
      "Klubnik to Moore, gain of 27 yards.",
    ]);
    const lines = buildCommentary(rec);
    expect(lines.every((l) => l.team === "home")).toBe(true);
    expect(lines.at(-1)!.final).toBe(true);
  });

  it("calls a sack, and a run's read and broken tackle", () => {
    const sack = play([
      { tick: 0, event: "snap" },
      { tick: 2, event: "tackle", player: "WDE", on: "QB" },
      { tick: 2, event: "sack", yards: -7.4 },
    ]);
    expect(texts(sack).slice(1)).toEqual(["Heldt gets to Klubnik... SACKED!", "Klubnik is sacked for a loss of 7 yards."]);

    const run = play([
      { tick: 0, event: "snap" },
      { tick: 1, event: "handoff", from: "QB", to: "RB" },
      { tick: 1, event: "read", player: "RB", read: "bounce" },
      { tick: 2, event: "evasion", player: "RB", on: "FS", move: "juke", won: true },
      { tick: 2, event: "evasion", player: "RB", on: "WCB", move: "spin", won: false },
      { tick: 3, event: "broken_tackle", player: "WCB", on: "RB" },
      { tick: 4, event: "tackle", player: "FS", on: "RB" },
      { tick: 4, event: "tackled", yards: -1 },
    ]);
    expect(texts(run).slice(1)).toEqual([
      "Klubnik hands it off to Randall.",
      "Randall bounces it outside!",
      "Randall jukes Barnes!", // the lost spin isn't a highlight
      "Randall breaks the tackle of Lukus!",
      "Barnes brings Randall down.",
      "Randall runs for a loss of 1 yard.",
    ]);
  });

  it("switches to the defense's colors on an interception", () => {
    const rec = play([
      { tick: 0, event: "snap" },
      { tick: 1, event: "throw", from: "QB", to: "X", air_yards: 25, openness: 0.1 },
      { tick: 2, event: "interception", player: "WCB", air_yards: 25 },
      { tick: 4, event: "tackled", yards: 0 },
    ]);
    const lines = buildCommentary(rec);
    expect(lines.map((l) => l.text)).toEqual([
      "Klubnik takes the snap.",
      "Klubnik lets it fly deep, he's looking for Moore... Into tight coverage!",
      "INTERCEPTED! Lukus picks it off!",
      "Klubnik's pass is intercepted by Lukus.",
    ]);
    expect(lines.map((l) => l.team)).toEqual(["home", "home", "away", "away"]);
    expect(lines[2]!.big).toBe(true);
  });

  it("shows the last line that has started", () => {
    const lines = [{ frame: 2, text: "a", team: "home" as const }, { frame: 5, text: "b", team: "home" as const }];
    expect(lineAt(lines, 0)).toBe(-1);
    expect(lineAt(lines, 2)).toBe(0);
    expect(lineAt(lines, 9)).toBe(1);
  });
});

describe("commentary helpers", () => {
  it("names spots from the team's side of the field", () => {
    const rec = play([]);
    expect(spotName(rec, "home", 36)).toBe("his own 26");
    expect(spotName(rec, "home", 60)).toBe("midfield");
    expect(spotName(rec, "home", 80)).toBe("the RIV 30");
    expect(spotName(rec, "away", 80)).toBe("his own 30");
    expect(spotName(rec, "home", 112)).toBe("the end zone");
  });

  it("words gains and names", () => {
    expect(gain(27.2)).toBe("gain of 27 yards");
    expect(gain(1)).toBe("gain of 1 yard");
    expect(gain(0.3)).toBe("no gain");
    expect(forYards(-3)).toBe("a loss of 3 yards");
    expect(forYards(12)).toBe("12 yards");
    expect(lastName("T. Moore")).toBe("Moore");
    expect(lastName("M. Jones Jr.")).toBe("Jones Jr.");
  });

  it("keeps white text readable on light and dark team colors", () => {
    expect(teamColors("#522D80")).toMatchObject({ base: "#522d80", ink: "#ffffff" });
    expect(teamColors("#FFD400").ink).toBe("#111111");
    const dark = teamColors("#FFD400").dark;
    expect(parseInt(dark.slice(1, 3), 16)).toBeLessThan(80); // a light color still gets a dark band
  });
});
