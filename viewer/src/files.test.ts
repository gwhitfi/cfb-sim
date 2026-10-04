import { describe, expect, it } from "vitest";
import { gameLogTitle, groupByFolder, humanize, newestFirst, playFileTitle } from "./files";
import { FACTORY, parseDefaults } from "./settings";
import type { TeamInfo } from "./plays";

const teams: TeamInfo[] = [
  { id: "clemson_2018", name: "Clemson 2018", abbr: "CLEM", defense: [{ id: "43_over_cover1", name: "4-3 Over Cover 1", type: "defense", formation: "43_over" }] },
  { id: "clemson_2025", name: "Clemson 2025", abbr: "CLEM", offense: [{ id: "trips_stick", name: "Trips Stick", type: "pass", formation: "trips" }] },
  { id: "max_a", name: "Test Max A", abbr: "MAXA" },
];

describe("replay titles", () => {
  it("names a Plays mode file from the server's teams and plays", () => {
    expect(playFileTitle("out/plays/clemson_2025_trips_stick_vs_clemson_2018_43_over_cover1_seed32.jsonl", teams)).toEqual({
      title: "Trips Stick vs 4-3 Over Cover 1",
      sub: "Clemson 2025 vs Clemson 2018 · seed 32",
    });
    expect(playFileTitle("out/plays/max_a_smash_vs_empty_seed4.jsonl", teams)).toEqual({
      title: "Smash vs empty field",
      sub: "Test Max A · seed 4",
    });
  });

  it("falls back to the file's name", () => {
    expect(playFileTitle("out/plays/clemson_2025_trips_stick_vs_clemson_2018_43_over_cover1_seed32.jsonl")).toEqual({
      title: "Clemson 2025 Trips Stick vs Clemson 2018 43 Over Cover1",
      sub: "seed 32",
    });
    expect(playFileTitle("forty.jsonl")).toEqual({ title: "Forty", sub: "" });
    expect(humanize("seed1775_11.5yd")).toBe("Seed1775 11.5yd");
  });

  it("names a game log: away at home, the time it was played, the seed", () => {
    expect(gameLogTitle("out/games/clemson_2018_vs_clemson_2025_20261003_174538_seed782899678.gamelog.jsonl", teams)).toEqual({
      title: "Clemson 2025 at Clemson 2018",
      sub: "Oct 3, 2026 5:45 PM · seed 782899678",
    });
    expect(gameLogTitle("out/games/r80_a_vs_r80_b_20261003_001500_seed1.gamelog.jsonl").title).toBe("R80 B at R80 A");
    expect(gameLogTitle("game.gamelog.jsonl").title).toBe("Game");
  });

  it("groups by folder with the plays folder first, and sorts logs newest first", () => {
    expect(groupByFolder(["forty.jsonl", "out/iz/a.jsonl", "out/plays/b.jsonl"])).toEqual([
      ["out/plays", ["out/plays/b.jsonl"]],
      [".", ["forty.jsonl"]],
      ["out/iz", ["out/iz/a.jsonl"]],
    ]);
    expect(newestFirst(["a_vs_b_20261003_051815_seed1.gamelog.jsonl", "x_vs_y_20261003_174538_seed2.gamelog.jsonl"])[0]).toMatch(/^x_vs_y/);
  });
});

describe("defaults", () => {
  it("fills anything missing or wrong from the factory defaults", () => {
    expect(parseDefaults(null)).toEqual(FACTORY);
    const d = parseDefaults({ view: "vertical", behind: "sideways", speed: 2, zoom: "zoomy", auto: true, open: { "sec-events": true, "sec-stats": "yes" } });
    expect(d.view).toBe("vertical");
    expect(d.behind).toBe("offense");
    expect(d.speed).toBe(2);
    expect(d.zoom).toBe("fixed");
    expect(parseDefaults({ speed: 3 }).speed).toBe(1);
    expect(d.auto).toBe(true);
    expect(d.open["sec-events"]).toBe(true);
    expect(d.open["sec-stats"]).toBe(true);
    expect(d.open["sec-player"]).toBe(false);
  });
});
