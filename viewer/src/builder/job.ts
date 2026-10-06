// The job strip's words: how far a run has got, about how long is left, and
// how it ended. Pure, so it's tested.

// "under a second", "about 40 s", "about 3 min".
export function aboutTime(seconds: number): string {
  if (seconds < 1) return "under a second";
  if (seconds < 90) return `about ${Math.round(seconds)} s`;
  return `about ${Math.round(seconds / 60)} min`;
}

// "12 of 140 cells · about 30 s left". The time left is the pace so far
// carried on; it isn't guessed until a few cells are in.
export function progressText(done: number, total: number, elapsedSeconds: number): string {
  const cells = `${done} of ${total} cells`;
  if (done < 2 || done >= total || elapsedSeconds < 0.5) return cells;
  return `${cells} · ${aboutTime((elapsedSeconds / done) * (total - done))} left`;
}

// A saved file's name without its folder.
export function shortFile(path: string): string {
  return path.split("/").pop() ?? path;
}

// How a run ended: "Done: 140 cells in 4.2 s", "Cancelled at 30 cells"; and the file it was saved to.
export function doneText(d: { cells: number; cancelled: boolean; file?: string }, seconds: number): { text: string; file: string | null } {
  const text = d.cancelled ? `Cancelled at ${d.cells} cells` : `Done: ${d.cells} cells in ${seconds.toFixed(1)} s`;
  return { text, file: d.file ?? null };
}
