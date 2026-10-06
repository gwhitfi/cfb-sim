// Sorting and exporting the matrix: pure, so it's tested.

import type { Metric, Reading } from "./results";
import { formatValue } from "./results";

export type SortDir = "asc" | "desc";

// The indices 0..n-1 ordered by `valueAt` (nulls last, ties in the original
// order). With no direction, the original order.
export function sortIndices(n: number, valueAt: (i: number) => number | null, dir: SortDir | null): number[] {
  const idx = Array.from({ length: n }, (_, i) => i);
  if (!dir) return idx;
  const sign = dir === "asc" ? 1 : -1;
  return idx.sort((a, b) => {
    const va = valueAt(a), vb = valueAt(b);
    if (va === null && vb === null) return a - b;
    if (va === null) return 1;
    if (vb === null) return -1;
    return va === vb ? a - b : sign * (va - vb);
  });
}

// Click a header: none → descending → ascending → none.
export function nextDir(current: SortDir | null): SortDir | null {
  return current === null ? "desc" : current === "desc" ? "asc" : null;
}

function csvField(text: string): string {
  return /[",\n]/.test(text) ? `"${text.replace(/"/g, '""')}"` : text;
}

// The visible part of the matrix for one metric as CSV: a header of column
// names, then a row per play with its cells and total, and a totals row.
// `read(r, c)`: the cell at row r, column c; r or c of -1 is the total.
export function toCsv(
  metric: Metric,
  rows: { name: string }[],
  cols: { name: string }[],
  read: (r: number, c: number) => Reading | null,
  order: { rows: number[]; cols: number[] },
): string {
  const cell = (r: number, c: number) => {
    const v = read(r, c);
    return v ? formatValue({ ...metric, percent: false, digits: metric.percent ? 4 : 3 }, v.value) : "";
  };
  const lines = [["", ...order.cols.map((c) => cols[c]!.name), "All"].map(csvField).join(",")];
  for (const r of order.rows) {
    lines.push([rows[r]!.name, ...order.cols.map((c) => cell(r, c)), cell(r, -1)].map(csvField).join(","));
  }
  lines.push(["All", ...order.cols.map((c) => cell(-1, c)), cell(-1, -1)].map(csvField).join(","));
  return lines.join("\n") + "\n";
}
