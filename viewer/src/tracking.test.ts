import { describe, expect, it } from "vitest";
import { frameAtTick, parseTracking, STRIDE, Z } from "./tracking";

const header = {
  type: "header",
  format_version: 1,
  sim_version: "0.1.0",
  kind: "drill",
  name: "test",
  seed: 3,
  tick_rate: 20,
  teams: { home: { name: "Home", abbr: "HOME", color: "#C8102E" }, away: { name: "Away", abbr: "AWAY", color: "#1D4F91" } },
  details: {},
  players: [
    { id: "a", name: "A", label: "1", team: "home", jersey: 1, position: "WR", radius: 0.4 },
    { id: "b", name: "B", label: "2", team: "away", jersey: 2, position: "CB", radius: 0.42 },
  ],
  overlays: [{ kind: "text", layer: "marks", at: [10, 20], text: "start" }],
};

const tick = (n: number, x: number, extra: object = {}) => ({
  type: "tick",
  tick: n,
  p: [
    [x, 20, 1, 0, 0],
    [x + 5, 20, -1, 0, 180],
  ],
  ball: null,
  ...extra,
});

const file = (...lines: object[]) => lines.map((l) => JSON.stringify(l)).join("\n") + "\n";

describe("parseTracking", () => {
  it("reads the header, ticks, overlays, and events", () => {
    const rec = parseTracking(
      file(
        header,
        tick(0, 10),
        tick(1, 11, { dbg: [{ kind: "link", layer: "engagements", a: 0, b: 1 }] }),
        { type: "event", tick: 1, event: "split", player: "a", yards: 10, time: 1.5 },
        tick(2, 12, { ball: [12, 20, 1.5] }),
      ),
    );
    expect(rec.frameCount).toBe(3);
    expect(rec.header.players.map((p) => p.id)).toEqual(["a", "b"]);
    expect(rec.state.length).toBe(3 * 2 * STRIDE);
    expect(rec.state[(2 * 2 + 1) * STRIDE]).toBe(17); // frame 2, player b, x
    expect(rec.ball[1]).toBeNull();
    expect(Array.from(rec.ball[2]!)).toEqual([12, 20, 1.5]);
    expect(rec.debug[0]).toBeUndefined();
    expect(rec.debug[1]).toHaveLength(1);
    expect(rec.layers).toEqual(["engagements", "marks"]);
    expect(rec.events).toEqual([
      { tick: 1, frame: 1, event: "split", fields: { player: "a", yards: 10, time: 1.5 } },
    ]);
  });

  it("skips unknown line types and blank lines", () => {
    const rec = parseTracking(file(header, { type: "future_thing" }, tick(0, 10)) + "\n\n");
    expect(rec.frameCount).toBe(1);
  });

  it("explains that a game log isn't a tracking file", () => {
    const log = file({ type: "header", sim_version: "0.1.0", rules: "ncaa_fbs", seed: 1, home: "a", away: "b" });
    expect(() => parseTracking(log)).toThrow("this is a game log");
  });

  it("reads a sixth value (z) for a player in the air, 0 otherwise", () => {
    const rec = parseTracking(file(header, { ...tick(0, 10), p: [[1, 2, 0, 0, 0], [3, 4, 0, 0, 0, 0.42]] }));
    expect(rec.state[0 * STRIDE + Z]).toBe(0);
    expect(rec.state[1 * STRIDE + Z]).toBeCloseTo(0.42);
    expect(rec.state[1 * STRIDE]).toBe(3);
  });

  it("gives errors that name the line", () => {
    expect(() => parseTracking(file(tick(0, 10)))).toThrow("line 1: expected the header line first");
    expect(() => parseTracking(file(header, tick(0, 10), tick(0, 11)))).toThrow("line 3: tick 0 is not after tick 0");
    expect(() => parseTracking(file(header, { ...tick(0, 10), p: [[1, 2, 3, 4, 5]] }))).toThrow(
      "line 2: p: expected 2 players",
    );
    expect(() => parseTracking(file(header, tick(0, 10, { dbg: [{ kind: "blob", layer: "x" }] })))).toThrow(
      "line 2: dbg[0].kind: unknown shape 'blob'",
    );
    expect(() => parseTracking(file(header, tick(0, 10, { dbg: [{ kind: "ring", layer: "read", player: 7 }] })))).toThrow(
      "line 2: dbg[0].player: no player with index 7",
    );
    expect(() => parseTracking("{nope\n")).toThrow("line 1: not valid JSON");
    expect(() => parseTracking(file(header))).toThrow("the file has no tick lines");
  });
});

describe("frameAtTick", () => {
  it("finds the first frame at or after a tick, across gaps", () => {
    const ticks = Int32Array.from([0, 1, 2, 50, 51]);
    expect(frameAtTick(ticks, 0)).toBe(0);
    expect(frameAtTick(ticks, 2)).toBe(2);
    expect(frameAtTick(ticks, 10)).toBe(3);
    expect(frameAtTick(ticks, 99)).toBe(4);
  });
});
