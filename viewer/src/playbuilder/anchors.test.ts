import { describe, expect, it } from "vitest";
import {
  anchorJson, gapX, makeFrame, num, parseAnchor, placeSlots, playerFor, readOffense, resolveX, withX,
  type Anchor, type Spot,
} from "./anchors";

const mid = makeFrame("middle");
const spot = (name: string, x: number, y: number, onLine = y >= -1.5): Spot => ({ name, x, y, onLine });
// Gun Wide Trips: three receivers to the right, X split left.
const trips = [
  spot("LT", -3, -1), spot("LG", -1.5, -1), spot("C", 0, -1), spot("RG", 1.5, -1), spot("RT", 3, -1),
  spot("X", -18.67, -1), spot("Z", 18.67, -1), spot("H", 13.67, -2), spot("Y", 9.67, -2),
  spot("QB", 0, -5), spot("RB", -2, -5),
];
const ctxOf = (read?: ReturnType<typeof readOffense>) => ({ frame: mid, read, slotPos: () => undefined });

describe("landmark anchors", () => {
  it("puts the numbers 18.67 yd from the middle with the ball in the middle", () => {
    const a: Anchor = { kind: "numbers", side: "left", offset: 0 };
    expect(resolveX(a, ctxOf())).toBeCloseTo(-18.67, 2);
  });
  it("moves with the hash", () => {
    const a: Anchor = { kind: "sideline", side: "left", offset: 5 };
    expect(resolveX(a, { ...ctxOf(), frame: makeFrame("left") })).toBeCloseTo(-15, 2);
    expect(resolveX(a, { ...ctxOf(), frame: makeFrame("right") })).toBeCloseTo(-160 / 6 - 20 / 3 + 5, 6);
  });
  it("signs a relative side's offset toward its own sideline", () => {
    const strongRight = readOffense(trips, mid, ["passing_strength"], "");
    expect(strongRight.strong).toBe("right");
    expect(resolveX({ kind: "numbers", side: "strong", offset: 2 }, ctxOf(strongRight))).toBeCloseTo(20.67, 2);
    expect(resolveX({ kind: "numbers", side: "weak", offset: 2 }, ctxOf(strongRight))).toBeCloseTo(-20.67, 2);
  });
});

describe("reading the offense", () => {
  const read = readOffense(trips, mid, ["te", "passing_strength"], "");
  it("numbers receivers from the sideline in, and finds the backs", () => {
    expect(read.receivers.right).toEqual(["Z", "H", "Y"]);
    expect(read.receivers.left).toEqual(["X"]);
    expect(read.backs).toEqual(["QB"].filter(() => false).concat(["RB"]));
    expect(playerFor(read, "#2_strong")).toBe("H");
    expect(playerFor(read, "back_1")).toBe("RB");
  });
  it("takes the strength calls in order", () => {
    expect(readOffense(trips, mid, ["te", "passing_strength"], "").strong).toBe("right");
    expect(readOffense(trips, mid, ["boundary"], "").strong).toBe("left");
    expect(readOffense(trips, mid, ["passing_strength"], "left").strong).toBe("left");
  });
  it("finds the gaps", () => {
    expect(gapX(read, "right", "A")).toBeCloseTo(0.75);
    expect(gapX(read, "right", "B")).toBeCloseTo(2.25);
    expect(gapX(read, "left", "C")).toBeCloseTo(-4);
  });
  it("fails without a guard and a tackle", () => {
    expect(() => readOffense(trips.filter((s) => s.name !== "RT"), mid, [], "")).toThrow(/right/);
  });
});

describe("defensive anchors", () => {
  const read = readOffense(trips, mid, ["passing_strength"], "");
  it("lines up on the line by technique", () => {
    expect(resolveX({ kind: "tech", ref: "3", side: "strong", offset: 0 }, ctxOf(read))).toBeCloseTo(2);
    expect(resolveX({ kind: "tech", ref: "5", side: "strong", offset: 0 }, ctxOf(read))).toBeCloseTo(3.5);
    expect(resolveX({ kind: "tech", ref: "1", side: "weak", offset: 0 }, ctxOf(read))).toBeCloseTo(-0.75);
  });
  it("lines up on a receiver, offset toward his sideline", () => {
    expect(resolveX({ kind: "over", ref: "#1", side: "strong", offset: 1 }, ctxOf(read))).toBeCloseTo(19.67, 2);
    expect(resolveX({ kind: "over", ref: "#1", side: "weak", offset: 1 }, ctxOf(read))).toBeCloseTo(-19.67, 2);
  });
  it("needs the offense", () => {
    expect(() => resolveX({ kind: "tech", ref: "3", side: "strong", offset: 0 }, ctxOf())).toThrow();
  });
});

describe("placeSlots", () => {
  it("resolves slot anchors in any order", () => {
    const placed = placeSlots(
      [
        { name: "Y", x: { kind: "slot", ref: "H", offset: -4 }, y: -2 },
        { name: "H", x: { kind: "slot", ref: "Z", offset: -5 }, y: -2 },
        { name: "Z", x: { kind: "numbers", side: "right", offset: 0 }, y: -1 },
      ],
      { frame: mid },
    );
    expect(placed.Y!.x).toBeCloseTo(9.67, 2);
  });
  it("catches a loop and a missing slot", () => {
    const loop = [
      { name: "A", x: { kind: "slot", ref: "B", offset: 1 } as Anchor, y: 0 },
      { name: "B", x: { kind: "slot", ref: "A", offset: 1 } as Anchor, y: 0 },
    ];
    expect(() => placeSlots(loop, { frame: mid })).toThrow(/itself/);
    expect(() => placeSlots([{ name: "A", x: { kind: "slot", ref: "Q", offset: 0 }, y: 0 }], { frame: mid })).toThrow(/isn't placed/);
  });
});

describe("withX (dragging)", () => {
  const read = readOffense(trips, mid, ["passing_strength"], "");
  const ctx = { frame: mid, read, slotPos: (n: string) => (n === "Z" ? { x: 18.67, y: -1 } : undefined) };
  it("moves a number, snapped", () => {
    expect(withX(num(1), 2.3, ctx)).toEqual(num(2.5));
    expect(withX(num(1), 2.3, ctx, 0.1)).toEqual(num(2.3));
  });
  it("keeps the landmark and changes the offset", () => {
    expect(withX({ kind: "slot", ref: "Z", offset: -5 }, 12.67, ctx)).toEqual({ kind: "slot", ref: "Z", offset: -6 });
    expect(withX({ kind: "numbers", side: "left", offset: 0 }, -17.67, ctx)).toEqual({ kind: "numbers", side: "left", offset: 1 });
  });
  it("signs a relative side's offset toward its sideline", () => {
    // Weak is left here, so moving out (more negative x) is a + offset.
    expect(withX({ kind: "over", ref: "#1", side: "weak", offset: 0 }, -20.67, ctx)).toEqual({ kind: "over", ref: "#1", side: "weak", offset: 2 });
  });
});

describe("anchor JSON", () => {
  it("round-trips the data files' forms", () => {
    const forms: unknown[] = [
      2.5,
      { from: "numbers", side: "left", offset: 0 },
      { from: "ball", offset: 0 },
      { from: "Z", offset: -5 },
      { from: "gap_B", side: "right", offset: 1 },
      { tech: "3", side: "strong" },
      { over: "#1", side: "weak", offset: 1 },
    ];
    for (const f of forms) {
      const a = parseAnchor(f);
      expect(anchorJson(a)).toEqual(f === 2.5 ? 2.5 : f);
    }
    expect(anchorJson(num(0))).toBe(0);
  });
});
