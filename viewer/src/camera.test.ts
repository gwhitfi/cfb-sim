import { describe, expect, it } from "vitest";
import { Camera, FIELD_LENGTH, FIELD_WIDTH, type Turns } from "./camera";

function camera(turns: Turns): Camera {
  const cam = new Camera();
  cam.resize(800, 600);
  cam.turns = turns;
  cam.scale = 10;
  cam.cx = 50;
  cam.cy = 20;
  return cam;
}

describe("Camera", () => {
  it("maps field directions to the screen for each turn", () => {
    // [turns, where field +x points, where field +y points], as screen deltas.
    const cases: [Turns, [number, number], [number, number]][] = [
      [0, [10, 0], [0, -10]], // +x right, +y up
      [1, [0, -10], [-10, 0]], // +x up, +y left
      [2, [-10, 0], [0, 10]],
      [3, [0, 10], [10, 0]], // +x down, +y right
    ];
    for (const [turns, xDir, yDir] of cases) {
      const cam = camera(turns);
      const [ox, oy] = cam.toScreen(50, 20);
      expect([ox, oy]).toEqual([400, 300]);
      const [ax, ay] = cam.toScreen(51, 20);
      expect([ax - ox, ay - oy]).toEqual(xDir);
      const [bx, by] = cam.toScreen(50, 21);
      expect([bx - ox, by - oy]).toEqual(yDir);
    }
  });

  it("round-trips screen and field points, and pans with the drag", () => {
    for (const turns of [0, 1, 2, 3] as Turns[]) {
      const cam = camera(turns);
      const [sx, sy] = cam.toScreen(63.5, 12.25);
      const [x, y] = cam.toField(sx, sy);
      expect(x).toBeCloseTo(63.5, 9);
      expect(y).toBeCloseTo(12.25, 9);

      // Dragging moves the field point under the cursor with it.
      const before = cam.toField(100, 100);
      cam.panPixels(30, -20);
      const after = cam.toField(130, 80);
      expect(after[0]).toBeCloseTo(before[0], 9);
      expect(after[1]).toBeCloseTo(before[1], 9);
    }
  });

  it("fits a rectangle with its long side along the screen's long side", () => {
    const cam = camera(1);
    cam.fit(0, 0, 100, 40); // +x up: 100 yd tall, 40 wide
    expect(cam.scale).toBeCloseTo(6); // 600 px / 100 yd
  });

  it("frames the field's width across the screen in the vertical view", () => {
    const cam = camera(1);
    cam.fitWidthAt(35);
    expect(cam.cx).toBe(35);
    const [left] = cam.toScreen(35, FIELD_WIDTH + 3); // +y is left when +x is up
    const [right] = cam.toScreen(35, -3);
    expect(left).toBeCloseTo(0);
    expect(right).toBeCloseTo(800);
  });
});

describe("Camera.frameOn (fixed zoom)", () => {
  it("keeps the zoom and follows the point, without showing far past the field", () => {
    const cam = camera(0); // 800 x 600: the field's width runs up the 600 px
    cam.frameOn(50, 20, 30);
    expect(cam.scale).toBeCloseTo(20); // 30 yd across 600 px
    expect([cam.cx, cam.cy]).toEqual([50, 20]);
    // Near a corner: it stops at the field's margins (3 yd).
    cam.frameOn(0, 0, 30);
    expect(cam.cx).toBeCloseTo(20 - 3);
    expect(cam.cy).toBeCloseTo(15 - 3);
    cam.frameOn(FIELD_LENGTH, FIELD_WIDTH, 30);
    expect(cam.cx).toBeCloseTo(FIELD_LENGTH + 3 - 20);
  });

  it("centers the field across when the view is wider than it", () => {
    const cam = camera(1); // vertical: the field's width runs across the 800 px
    cam.frameOn(50, 5, 70);
    expect(cam.scale).toBeCloseTo(800 / 70);
    expect(cam.cy).toBeCloseTo(FIELD_WIDTH / 2);
    expect(cam.cx).toBe(50);
  });
});
