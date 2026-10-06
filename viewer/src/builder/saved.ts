// Saved matrix runs (out/matrix/*.json on the server), described for a menu.

export interface SavedRun {
  file: string;
  offense: string;
  defense: string;
  runs: number;
  rows: number;
  cols: number;
  cells: number;
  cancelled: boolean;
  kind?: string; // "kicks" for a kick matrix (missing: plays)
}

// "2026-10-05 19:57" from a file name that starts YYYYMMDD_HHMMSS (the time it was run).
export function whenOf(file: string): string {
  const name = file.split("/").pop() ?? file;
  const m = /^(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})\d{2}_/.exec(name);
  return m ? `${m[1]}-${m[2]}-${m[3]} ${m[4]}:${m[5]}` : name;
}

export function describeSaved(r: SavedRun): string {
  const done = r.cancelled ? ` (cancelled at ${r.cells}/${r.rows * r.cols})` : "";
  const what = r.kind === "kicks" ? `kicks · ${r.offense} kicking vs ${r.defense}` : `${r.offense} vs ${r.defense || "empty field"}`;
  return `${whenOf(r.file)} · ${what} · ${r.rows}×${r.cols}, ${r.runs} runs${done}`;
}
