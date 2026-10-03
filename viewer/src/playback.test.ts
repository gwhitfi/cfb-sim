import { describe, expect, it } from "vitest";
import { ballAt, ballHeading, lerpAngleDeg, Playback } from "./playback";
import { parseTracking } from "./tracking";

// One player running +x at 1 yd per tick, facing swinging across ±180°.
function recording() {
  const lines = [
    { type: "header", format_version: 1, tick_rate: 20, players: [{ id: "a", team: "home" }] },
    { type: "tick", tick: 0, p: [[10, 5, 20, 0, 170]], ball: [10, 5, 0] },
    { type: "tick", tick: 1, p: [[11, 5, 20, 0, -170]], ball: [11, 5, 2] },
    { type: "tick", tick: 2, p: [[12, 5, 20, 0, -170]], ball: null },
    { type: "tick", tick: 10, p: [[13, 5, 20, 0, -170]], ball: null },
  ];
  return parseTracking(lines.map((l) => JSON.stringify(l)).join("\n"));
}

describe("Playback", () => {
  it("interpolates position and facing between frames", () => {
    const pb = new Playback(recording());
    pb.seek(0.5);
    const p = pb.player(0);
    expect(p.x).toBeCloseTo(10.5);
    expect(Math.abs(p.facingDeg)).toBeCloseTo(180); // the short way round, not through 0
  });

  it("advances by wall time times speed, and stops at the end", () => {
    const pb = new Playback(recording());
    pb.togglePlay();
    pb.advance(0.025); // half a tick at 20 Hz
    expect(pb.frame).toBeCloseTo(0.5);
    pb.speed = 4;
    pb.advance(10);
    expect(pb.frame).toBe(3);
    expect(pb.playing).toBe(false);
    pb.togglePlay(); // playing again from the end restarts
    expect(pb.frame).toBe(0);
  });

  it("steps whole frames and reports ticks and seconds across gaps", () => {
    const pb = new Playback(recording());
    pb.seek(1.4);
    pb.step(1);
    expect(pb.frame).toBe(2);
    pb.step(1);
    expect(pb.tick).toBe(10);
    expect(pb.seconds).toBeCloseTo(0.5);
    pb.step(-1);
    pb.step(-1);
    pb.step(-1);
    pb.step(-1);
    expect(pb.frame).toBe(0);
  });

  it("interpolates the ball, and holds it when it goes dead", () => {
    const rec = recording();
    expect(ballAt(rec, 0.5)).toEqual([10.5, 5, 1]);
    expect(ballAt(rec, 1.5)).toEqual([11, 5, 2]);
    expect(ballAt(rec, 2.5)).toBeNull();
  });
});

describe("lerpAngleDeg", () => {
  it("goes the short way round", () => {
    expect(lerpAngleDeg(10, 50, 0.5)).toBe(30);
    expect(lerpAngleDeg(170, -170, 0.5)).toBe(180);
    expect(lerpAngleDeg(-170, 170, 0.5)).toBe(-180);
  });
});

describe("ballHeading", () => {
  const lines = (balls: ([number, number, number] | null)[]) =>
    parseTracking(
      [
        JSON.stringify({ type: "header", format_version: 1, tick_rate: 20, players: [{ id: "a", team: "home" }] }),
        ...balls.map((ball, t) => JSON.stringify({ type: "tick", tick: t, p: [[0, 0, 0, 0, 0]], ball })),
      ].join("\n"),
    );

  it("points the way the ball is going", () => {
    const rec = lines([[10, 10, 2], [11, 11, 2.5], [12, 12, 2.8]]);
    const [dx, dy] = ballHeading(rec, 0.5)!;
    expect(dx).toBeCloseTo(Math.SQRT1_2);
    expect(dy).toBeCloseTo(Math.SQRT1_2);
  });

  it("keeps the way it last moved once it stops, and is null before it has moved", () => {
    const rec = lines([[10, 10, 0], [10, 10, 0], [10, 9, 1], [10, 9, 1], [10, 9, 1]]);
    expect(ballHeading(rec, 0)).toBeNull();
    expect(ballHeading(rec, 4)).toEqual([0, -1]);
  });
});
