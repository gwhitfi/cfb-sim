import { describe, expect, it } from "vitest";
import { ballAt, ballHeading, lerpAngleDeg, Playback, snapToWhistle } from "./playback";
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

  it("plays a file recorded every 4 ticks at its own rate", () => {
    const lines = [
      { type: "header", format_version: 2, tick_rate: 20, frame_ticks: 4, players: [{ id: "a", team: "home" }] },
      { type: "tick", tick: 0, p: [[0, 5, 0, 0, 0]], ball: null },
      { type: "tick", tick: 4, p: [[1, 5, 0, 0, 0]], ball: null },
      { type: "tick", tick: 8, p: [[2, 5, 0, 0, 0]], ball: null },
    ];
    const pb = new Playback(parseTracking(lines.map((l) => JSON.stringify(l)).join("\n")));
    pb.togglePlay();
    pb.advance(0.2); // one frame at 5 a second
    expect(pb.frame).toBeCloseTo(1);
    expect(pb.seconds).toBeCloseTo(0.2);
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

// A play: huddle (ticks 0-2), the snap at 3, a tackle try at 5, the whistle at
// 6, then the pull-up (7-9).
function play(events: { tick: number; event: string }[]) {
  const lines: object[] = [{ type: "header", format_version: 1, tick_rate: 20, players: [{ id: "a", team: "home" }] }];
  for (let t = 0; t < 10; t++) lines.push({ type: "tick", tick: t, p: [[t, 5, 0, 0, 0]], ball: null });
  for (const e of events) lines.push({ type: "event", ...e });
  return parseTracking(lines.map((l) => JSON.stringify(l)).join("\n"));
}

describe("dev mode (snap to whistle)", () => {
  it("finds the snap and the dead ball", () => {
    const rec = play([
      { tick: 3, event: "snap" },
      { tick: 5, event: "drag_down" },
      { tick: 6, event: "tackled" },
    ]);
    expect(snapToWhistle(rec)).toEqual([3, 6]);
  });

  it("plays a whole drill (no snap), and a play with no whistle to the end", () => {
    expect(snapToWhistle(play([]))).toBeNull();
    expect(snapToWhistle(play([{ tick: 3, event: "snap" }]))).toEqual([3, 9]);
    expect(snapToWhistle(play([{ tick: 3, event: "snap" }, { tick: 8, event: "touchback" }]))).toEqual([3, 8]);
  });

  it("keeps playback inside the range", () => {
    const pb = new Playback(play([{ tick: 3, event: "snap" }, { tick: 6, event: "incomplete" }]));
    pb.setRange(snapToWhistle(pb.rec));
    expect(pb.frame).toBe(3); // moved up to the snap
    pb.seek(0);
    expect(pb.frame).toBe(3);
    pb.togglePlay();
    pb.advance(10);
    expect(pb.frame).toBe(6); // stops at the whistle
    expect(pb.playing).toBe(false);
    pb.togglePlay(); // again from the snap
    expect(pb.frame).toBe(3);
    pb.setRange(null);
    pb.seek(9);
    expect(pb.frame).toBe(9);
  });
});
