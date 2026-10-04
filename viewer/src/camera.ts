// Maps field coordinates (yards; +x toward the right end zone, +y toward the
// top sideline) to canvas pixels and back. Screen y grows downward, so field y
// is flipped.
//
// The view can be turned in quarter turns: `turns` = 0 is the sideline view
// (+x to the right), 1 turns the field a quarter counterclockwise (+x up), 3 a
// quarter clockwise (+x down). The vertical view uses 1 or 3 so one team
// attacks up the screen.

export const FIELD_LENGTH = 120;
export const FIELD_WIDTH = 160 / 3; // 53.33 yd
const MARGIN = 3; // yd of sideline area shown around the field

export type Turns = 0 | 1 | 2 | 3;

// Unit vectors (in "screen up is +" coordinates) for each quarter turn: where
// field +x and +y point. Exact integers, so a round trip is exact.
const AXES: Record<Turns, { x: [number, number]; y: [number, number] }> = {
  0: { x: [1, 0], y: [0, 1] },
  1: { x: [0, 1], y: [-1, 0] },
  2: { x: [-1, 0], y: [0, -1] },
  3: { x: [0, -1], y: [1, 0] },
};

export class Camera {
  // Canvas size in CSS pixels.
  width = 1;
  height = 1;
  // Pixels per yard, and the field point at the center of the canvas.
  scale = 10;
  cx = FIELD_LENGTH / 2;
  cy = FIELD_WIDTH / 2;
  turns: Turns = 0;

  resize(width: number, height: number): void {
    this.width = width;
    this.height = height;
  }

  // The screen rotation, in radians clockwise (the canvas's sense), that
  // field-aligned text and shapes need on top of their own rotation.
  get angle(): number {
    return (-this.turns * Math.PI) / 2;
  }

  // Fits a field rectangle (in yards) into the canvas.
  fit(minX: number, minY: number, maxX: number, maxY: number): void {
    this.cx = (minX + maxX) / 2;
    this.cy = (minY + maxY) / 2;
    const [w, h] = this.turns % 2 === 0 ? [maxX - minX, maxY - minY] : [maxY - minY, maxX - minX];
    this.scale = Math.min(this.width / w, this.height / h);
  }

  fitField(): void {
    this.fit(-MARGIN, -MARGIN, FIELD_LENGTH + MARGIN, FIELD_WIDTH + MARGIN);
  }

  // The vertical view's framing: the whole width of the field (sideline to
  // sideline) across the screen, centered on field x = `x`.
  fitWidthAt(x: number): void {
    const across = FIELD_WIDTH + 2 * MARGIN;
    this.scale = (this.turns % 2 === 0 ? this.height : this.width) / across;
    this.cx = x;
    this.cy = FIELD_WIDTH / 2;
  }

  toScreen(x: number, y: number): [number, number] {
    const { x: ax, y: ay } = AXES[this.turns];
    const dx = x - this.cx;
    const dy = y - this.cy;
    const u = dx * ax[0] + dy * ay[0]; // right on screen
    const v = dx * ax[1] + dy * ay[1]; // up on screen
    return [this.width / 2 + u * this.scale, this.height / 2 - v * this.scale];
  }

  // Fixed zoom: `yardsAcross` yards of the field's width (sideline to
  // sideline) on the screen, centered on field point (x, y), but kept from
  // showing far past the field: along each field axis, if the view is wider
  // than the field (plus margins) it centers the field, otherwise it stops at
  // the margins.
  frameOn(x: number, y: number, yardsAcross: number): void {
    this.scale = this.acrossPixels / yardsAcross;
    const [alongX, alongY] =
      this.turns % 2 === 0 ? [this.width / this.scale, this.height / this.scale] : [this.height / this.scale, this.width / this.scale];
    const keep = (c: number, visible: number, size: number) => {
      const lo = visible / 2 - MARGIN;
      const hi = size + MARGIN - visible / 2;
      return lo >= hi ? size / 2 : Math.min(Math.max(c, lo), hi);
    };
    this.cx = keep(x, alongX, FIELD_LENGTH);
    this.cy = keep(y, alongY, FIELD_WIDTH);
  }

  // The field offset (in yards) that moves the view `yards` up the screen.
  upBy(yards: number): [number, number] {
    const { x: ax, y: ay } = AXES[this.turns];
    return [ax[1] * yards, ay[1] * yards];
  }

  // The screen dimension that runs across the field (sideline to sideline).
  get acrossPixels(): number {
    return this.turns % 2 === 0 ? this.height : this.width;
  }

  toField(sx: number, sy: number): [number, number] {
    const [dx, dy] = this.screenDelta(sx - this.width / 2, sy - this.height / 2);
    return [this.cx + dx, this.cy + dy];
  }

  // Zooms by `factor`, keeping the field point under (sx, sy) fixed on screen.
  zoomAt(sx: number, sy: number, factor: number): void {
    const [fx, fy] = this.toField(sx, sy);
    this.scale = Math.min(Math.max(this.scale * factor, 2), 200);
    const [nx, ny] = this.toField(sx, sy);
    this.cx += fx - nx;
    this.cy += fy - ny;
  }

  panPixels(dx: number, dy: number): void {
    const [fx, fy] = this.screenDelta(dx, dy);
    this.cx -= fx;
    this.cy -= fy;
  }

  // A screen-pixel offset (right, down) as a field offset in yards. The axes
  // are orthonormal, so the inverse is the transpose.
  private screenDelta(px: number, py: number): [number, number] {
    const { x: ax, y: ay } = AXES[this.turns];
    const u = px / this.scale;
    const v = -py / this.scale;
    return [u * ax[0] + v * ax[1], u * ay[0] + v * ay[1]];
  }
}
