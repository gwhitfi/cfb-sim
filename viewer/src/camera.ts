// Maps field coordinates (yards; +x toward the right end zone, +y toward the
// top sideline) to canvas pixels and back. Screen y grows downward, so field y
// is flipped.

export const FIELD_LENGTH = 120;
export const FIELD_WIDTH = 160 / 3; // 53.33 yd
const MARGIN = 3; // yd of sideline area shown around the field

export class Camera {
  // Canvas size in CSS pixels.
  width = 1;
  height = 1;
  // Pixels per yard, and the field point at the center of the canvas.
  scale = 10;
  cx = FIELD_LENGTH / 2;
  cy = FIELD_WIDTH / 2;

  resize(width: number, height: number): void {
    this.width = width;
    this.height = height;
  }

  // Fits a field rectangle (in yards) into the canvas.
  fit(minX: number, minY: number, maxX: number, maxY: number): void {
    this.cx = (minX + maxX) / 2;
    this.cy = (minY + maxY) / 2;
    this.scale = Math.min(this.width / (maxX - minX), this.height / (maxY - minY));
  }

  fitField(): void {
    this.fit(-MARGIN, -MARGIN, FIELD_LENGTH + MARGIN, FIELD_WIDTH + MARGIN);
  }

  toScreen(x: number, y: number): [number, number] {
    return [this.width / 2 + (x - this.cx) * this.scale, this.height / 2 - (y - this.cy) * this.scale];
  }

  toField(sx: number, sy: number): [number, number] {
    return [this.cx + (sx - this.width / 2) / this.scale, this.cy - (sy - this.height / 2) / this.scale];
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
    this.cx -= dx / this.scale;
    this.cy += dy / this.scale;
  }
}
