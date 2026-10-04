import { describe, expect, it } from "vitest";
import { eventDetails, eventTime } from "./events";
import type { TrackingEvent } from "./tracking";

const ev = (event: string, fields: Record<string, unknown>, tick = 0): TrackingEvent => ({ tick, frame: 0, event, fields });

describe("event details", () => {
  const names = new Map([["SAM", "J. Doe"], ["RB", "T. Leigh"]]);

  it("lists every field, with names next to slots", () => {
    const d = eventDetails(ev("tackle", { player: "SAM", on: "RB", quality: 0.734, score: 61.25, dive: true }), names);
    expect(d).toEqual([
      ["player", "SAM J. Doe"],
      ["on", "RB T. Leigh"],
      ["quality", "0.73"],
      ["score", "61.25"],
      ["dive", "true"],
    ]);
  });

  it("spells out a tackle's odds and a move's chance", () => {
    const d = eventDetails(ev("tackle", { odds: [0.6, 0.25, 0.1, 0.05] }), names);
    expect(d).toEqual([["odds", "clean 60% · drag 25% · broken 10% · missed 5%"]]);
    expect(eventDetails(ev("evasion", { chance: 0.314 }), names)).toEqual([["chance", "31%"]]);
  });

  it("times events from the snap", () => {
    expect(eventTime(ev("tackle", {}, 45), 20, 0, 20)).toBe("+1.25");
    expect(eventTime(ev("set", {}, 10), 20, 0, 20)).toBe("−0.50");
    expect(eventTime(ev("split", {}, 30), null, 10, 20)).toBe("1.00s");
  });
});
