// The Results tab: the matrix as a heat-map table that fills in as cells
// arrive. Colored against the run's own average or the calibration targets;
// sortable by any column, row, or total; rows and columns can be hidden (the
// totals then count only what's shown); a click opens a cell's detail card
// with Watch buttons; saved runs can be reopened and the shown metric exported.

import { detailSections } from "./detail";
import { el, load, save } from "./dom";
import { advantage, baselineFor, heatColor, type Baseline, type BaselineMode } from "./heat";
import { nextDir, sortIndices, toCsv, type SortDir } from "./order";
import {
  appliesTo, countOf, formatDelta, formatValue, halfWidth, isNoisy, metricByKey, metricsFor, reading, titleOf, totalKind,
  type Metric, type PlayKind, type Reading, type ResultGrid, type Sample, type Stats,
} from "./results";
import { describeSaved, type SavedRun } from "./saved";

const PREFS_KEY = "cfbsim.builder.results";

interface Prefs {
  metric: string; // the scrimmage metric shown
  kickMetric: string; // the kick metric shown
  baseline: BaselineMode;
  shows: "value" | "delta";
}

function loadPrefs(): Prefs {
  const r = (load(PREFS_KEY) ?? {}) as Partial<Prefs>;
  return {
    metric: metricByKey(r.metric ?? "", "scrimmage").key,
    kickMetric: metricByKey(r.kickMetric ?? "", "kicks").key,
    baseline: r.baseline === "targets" ? "targets" : "average",
    shows: r.shows === "delta" ? "delta" : "value",
  };
}

export interface ResultsHost {
  watch(sample: Sample, row: number, col: number): void;
  openSaved(file: string): void;
}

interface Sort {
  axis: "rows" | "cols";
  by: number | "total";
  dir: SortDir;
}

export class ResultsView {
  private grid: ResultGrid | null = null;
  private note = "";
  private prefs = loadPrefs();
  private sort: Sort | null = null;
  private hiddenRows = new Set<number>();
  private hiddenCols = new Set<number>();
  private selected: { r: number; c: number } | null = null;
  private saved: SavedRun[] = [];
  private head = el("div", { className: "results-head" });
  private layout = el("div", { className: "results-layout" });
  private frame = 0;

  constructor(root: HTMLElement, private host: ResultsHost) {
    root.append(this.head, this.layout);
    this.draw();
  }

  // A new run (null: none yet).
  setGrid(grid: ResultGrid | null, note = ""): void {
    this.grid = grid;
    this.note = note;
    this.sort = null;
    this.hiddenRows.clear();
    this.hiddenCols.clear();
    this.selected = null;
    this.draw();
  }

  setNote(note: string): void {
    this.note = note;
    this.schedule();
  }

  setSaved(runs: SavedRun[]): void {
    this.saved = runs;
    this.schedule();
  }

  // Cells arrive in a stream; redraw at most once a frame.
  schedule(): void {
    if (this.frame) return;
    this.frame = requestAnimationFrame(() => {
      this.frame = 0;
      this.draw();
    });
  }

  private setPref(patch: Partial<Prefs>): void {
    this.prefs = { ...this.prefs, ...patch };
    save(PREFS_KEY, this.prefs);
    this.draw();
  }

  // ---- what's shown

  // The rows and columns not hidden, in the sorted order.
  private order(m: Metric, g: ResultGrid): { rows: number[]; cols: number[] } {
    const rowsAll = g.info.rows.map((_, i) => i).filter((i) => !this.hiddenRows.has(i));
    const colsAll = g.info.cols.map((_, i) => i).filter((i) => !this.hiddenCols.has(i));
    const s = this.sort;
    const value = (r: number, c: number) => this.read(m, g, r, c, { rows: rowsAll, cols: colsAll }).reading?.value ?? null;
    let rows = rowsAll, cols = colsAll;
    if (s?.axis === "rows") {
      const idx = sortIndices(rowsAll.length, (i) => value(rowsAll[i]!, s.by === "total" ? -1 : s.by), s.dir);
      rows = idx.map((i) => rowsAll[i]!);
    } else if (s?.axis === "cols") {
      const idx = sortIndices(colsAll.length, (i) => value(s.by === "total" ? -1 : s.by, colsAll[i]!), s.dir);
      cols = idx.map((i) => colsAll[i]!);
    }
    return { rows, cols };
  }

  // The reading of the cell at (r, c); r or c of -1 is the total of that row/column (both: everything shown).
  // Column and grand totals count only the rows the metric means anything for.
  private read(m: Metric, g: ResultGrid, r: number, c: number, vis: { rows: number[]; cols: number[] }) {
    let stats: Stats | null;
    let kind: PlayKind | "both";
    if (r >= 0 && c >= 0) {
      stats = g.stats(r, c);
      kind = g.rowKind(r);
    } else if (r >= 0) {
      stats = g.total_of([r], vis.cols);
      kind = g.rowKind(r);
    } else {
      const rows = vis.rows.filter((i) => appliesTo(m, g.rowKind(i)));
      stats = g.total_of(rows, c >= 0 ? [c] : vis.cols);
      kind = totalKind(m);
    }
    return { stats, kind, reading: reading(m, stats, kind) };
  }

  // The baseline for each kind of row, from the cells shown.
  private baselines(m: Metric, g: ResultGrid, vis: { rows: number[]; cols: number[] }): Map<PlayKind, Baseline | null> {
    const out = new Map<PlayKind, Baseline | null>();
    for (const kind of g.kinds()) {
      if (!appliesTo(m, kind)) continue;
      const rows = vis.rows.filter((r) => g.rowKind(r) === kind);
      const values: number[] = [];
      for (const r of rows) for (const c of vis.cols) {
        const v = reading(m, g.stats(r, c), kind);
        if (v) values.push(v.value);
      }
      const avg = reading(m, g.total_of(rows, vis.cols), kind)?.value ?? null;
      out.set(kind, baselineFor(this.prefs.baseline, m, kind, g.info.targets, values, avg));
    }
    return out;
  }

  // ---- drawing

  private draw(): void {
    const g = this.grid;
    if (!g) {
      this.head.replaceChildren();
      this.layout.replaceChildren(el("p", { className: "muted", textContent: "No results yet. Set up a matchup and run it, or open a saved run." }, ...[]));
      this.head.append(this.savedMenu());
      return;
    }
    const domain = g.domain;
    const m = metricByKey(domain === "kicks" ? this.prefs.kickMetric : this.prefs.metric, domain);
    const { rows, cols } = this.order(m, g);
    const vis = { rows, cols };
    const baselines = this.baselines(m, g, vis);

    this.head.replaceChildren(
      el("strong", { textContent: titleOf(g.info) }),
      el("span", { className: "muted", textContent: `${g.info.runs} ${domain === "kicks" ? "kicks" : "runs"} per cell · ${g.done}/${g.total} cells` }),
      ...(this.note ? [el("span", { className: "muted", textContent: this.note })] : []),
      el("span", { className: "spacer" }),
      this.hiddenChip(g),
      this.select("Show", metricsFor(domain).map((x) => [x.key, x.label]), m.key, (v) => this.setPref(domain === "kicks" ? { kickMetric: v } : { metric: v })),
      this.select("Color against", [["average", "Run average"], ["targets", "Targets"]], this.prefs.baseline, (v) => this.setPref({ baseline: v as BaselineMode })),
      this.select("Cells show", [["value", "Value"], ["delta", "Difference"]], this.prefs.shows, (v) => this.setPref({ shows: v as Prefs["shows"] })),
      el("button", { textContent: "Export CSV", title: "The shown metric, as the table is sorted and filtered", onclick: () => this.exportCsv(m, g, rows, cols) }),
      this.savedMenu(),
    );

    const legend = el("div", { className: "legend" },
      el("span", { className: "muted", textContent: domain === "kicks" ? "returning team" : "defense" }),
      el("span", { className: "ramp" }),
      el("span", { className: "muted", textContent: domain === "kicks" ? "kicking team" : "offense" }),
      el("span", { className: "muted", textContent: this.baselineText(m, baselines) }),
    );

    this.layout.replaceChildren(
      el("div", { className: "results-main" }, legend, this.table(m, g, rows, cols, baselines, vis)),
      ...(this.selected ? [this.detail(m, g)] : []),
    );
  }

  private select(label: string, options: string[][], value: string, onChange: (v: string) => void): HTMLElement {
    const s = el("select", {}, ...options.map(([v, text]) => el("option", { value: v!, textContent: text!, selected: v === value })));
    s.addEventListener("change", () => onChange(s.value));
    return el("label", { className: "inline" }, label, s);
  }

  private hiddenChip(g: ResultGrid): HTMLElement {
    const n = this.hiddenRows.size + this.hiddenCols.size;
    if (n === 0 || !g) return el("span");
    return el("button", {
      textContent: `${n} hidden · show all`,
      title: "Totals count only what's shown",
      onclick: () => {
        this.hiddenRows.clear();
        this.hiddenCols.clear();
        this.draw();
      },
    });
  }

  private savedMenu(): HTMLElement {
    const s = el("select", { title: "Reopen a run saved by the server (out/matrix/)" },
      el("option", { value: "", textContent: this.saved.length ? "Saved runs…" : "No saved runs" }),
      ...this.saved.map((r) => el("option", { value: r.file, textContent: describeSaved(r) })));
    s.addEventListener("change", () => s.value && this.host.openSaved(s.value));
    return s;
  }

  private baselineText(m: Metric, b: Map<PlayKind, Baseline | null>): string {
    const parts: string[] = [];
    for (const [kind, base] of b) {
      if (!base) continue;
      const where = b.size > 1 ? `${kind.replace("_", " ")} rows ` : "";
      const what = base.source === "target" ? `target${base.note ? ` (${base.note})` : ""}` : this.prefs.baseline === "targets" ? "run average (no target)" : "run average";
      parts.push(`${where}centered on ${formatValue(m, base.center)}: ${what}`);
    }
    return parts.join(" · ");
  }

  private table(m: Metric, g: ResultGrid, rows: number[], cols: number[], baselines: Map<PlayKind, Baseline | null>, vis: { rows: number[]; cols: number[] }): HTMLElement {
    const arrow = (axis: Sort["axis"], by: number | "total") =>
      this.sort && this.sort.axis === axis && this.sort.by === by ? (this.sort.dir === "desc" ? " ▼" : " ▲") : "";
    const sortBy = (axis: Sort["axis"], by: number | "total") => {
      const same = this.sort && this.sort.axis === axis && this.sort.by === by;
      const dir = nextDir(same ? this.sort!.dir : null);
      this.sort = dir ? { axis, by, dir } : null;
      this.draw();
    };
    const hide = (set: Set<number>, i: number) =>
      el("span", {
        className: "hide", textContent: "×", title: "Hide (totals then leave it out)",
        onclick: (e: Event) => { e.stopPropagation(); set.add(i); this.draw(); },
      });

    const head = el("tr", {},
      el("th", { className: "corner", textContent: "" }),
      ...cols.map((c) => el("th", {
        className: "sortable", title: `${g.info.cols[c]!.id} · click to sort the rows by this column`,
        onclick: () => sortBy("rows", c),
      }, g.info.cols[c]!.name + arrow("rows", c), hide(this.hiddenCols, c))),
      el("th", { className: "total sortable", title: "Sort the rows by their totals", onclick: () => sortBy("rows", "total") }, "All" + arrow("rows", "total")),
    );

    const body = el("tbody");
    for (const r of rows) {
      const row = g.info.rows[r]!;
      const kind = g.rowKind(r);
      body.append(el("tr", {},
        el("th", { className: "rowhead sortable", title: `${row.id} · click to sort the columns by this row`, onclick: () => sortBy("cols", r) },
          row.name + arrow("cols", r), el("span", { className: `badge ${row.type}`, textContent: row.type }), hide(this.hiddenRows, r)),
        ...cols.map((c) => this.cell(m, g, r, c, baselines.get(kind) ?? null, vis)),
        this.cell(m, g, r, -1, null, vis),
      ));
    }
    body.append(el("tr", { className: "totals" },
      el("th", { className: "rowhead sortable", title: "Sort the columns by their totals", onclick: () => sortBy("cols", "total") }, "All" + arrow("cols", "total")),
      ...cols.map((c) => this.cell(m, g, -1, c, null, vis)),
      this.cell(m, g, -1, -1, null, vis),
    ));
    return el("table", { className: "matrix" }, el("thead", {}, head), body);
  }

  private cell(m: Metric, g: ResultGrid, r: number, c: number, base: Baseline | null, vis: { rows: number[]; cols: number[] }): HTMLElement {
    const total = r < 0 || c < 0;
    const { stats, reading: rd } = this.read(m, g, r, c, vis);
    const cls = total ? "total" : "";
    if (!stats) return el("td", { className: `pending ${cls}`.trim(), textContent: "…" });
    if (!total && g.cells[r]?.[c]?.applies === false) {
      return el("td", { className: "na", textContent: "·", title: `${g.info.cols[c]?.name} doesn't answer ${g.info.rows[r]?.name}, so nothing ran` });
    }
    const n = countOf(stats);
    const unit = g.domain === "kicks" ? "kicks" : "plays";
    if (!rd) return el("td", { className: cls, textContent: "–", title: `${n} ${unit}` });

    const hw = halfWidth(rd);
    const noisy = isNoisy(m, rd);
    const td = el("td", { className: [cls, noisy ? "noisy" : "", !total ? "pick" : ""].filter(Boolean).join(" ") });
    td.title = `${n} ${unit}` + (hw !== null ? ` · ±${formatValue({ ...m, digits: m.percent ? 1 : 2 }, hw)} (95%)` : "") + (noisy ? " · too few plays to trust" : "");
    td.textContent = this.prefs.shows === "delta" && base ? formatDelta(m, rd.value - base.center) : formatValue(m, rd.value);
    // A thin cell gets a fainter color: it may be luck.
    if (base && !total) td.style.background = heatColor(advantage(m, base, rd.value) * (noisy ? 0.5 : 1));
    if (noisy && hw !== null) td.append(el("span", { className: "hint", textContent: `±${formatValue({ ...m, digits: m.percent ? 0 : 1 }, hw)}` }));
    if (!total) {
      if (this.selected?.r === r && this.selected.c === c) td.classList.add("selected");
      td.addEventListener("click", () => {
        this.selected = { r, c };
        this.draw();
      });
    }
    return td;
  }

  // The detail card of the selected cell.
  private detail(m: Metric, g: ResultGrid): HTMLElement {
    const { r, c } = this.selected!;
    const cell = g.cells[r]?.[c];
    const row = g.info.rows[r]!, col = g.info.cols[c]!;
    const card = el("aside", { className: "detail" },
      el("div", { className: "detail-head" },
        el("strong", { textContent: `${row.name} vs ${col.name}` }),
        el("button", { textContent: "×", title: "Close", onclick: () => { this.selected = null; this.draw(); } }),
      ),
    );
    if (!cell) {
      card.append(el("p", { className: "muted", textContent: "This cell hasn't arrived yet." }));
      return card;
    }
    const samples = cell.samples ?? [];
    if (samples.length) {
      card.append(el("div", { className: "watch" },
        el("span", { className: "muted", textContent: "Watch the" }),
        ...samples.map((s) => el("button", {
          textContent: `${s.kind} (${s.yards.toFixed(1)} yd)`,
          title: `Seed ${s.seed}, ${s.hash} hash${s.flip ? ", flipped" : ""}: opens the play in the viewer`,
          onclick: () => this.host.watch(s, r, c),
        })),
      ));
    }
    for (const s of detailSections(cell.stats, g.rowKind(r), g.domain)) {
      card.append(el("h3", { textContent: s.title }),
        el("table", { className: "kv" }, el("tbody", {}, ...s.rows.map(([k, v]) => el("tr", {}, el("td", { textContent: k }), el("td", { textContent: v }))))));
    }
    void m;
    return card;
  }

  private exportCsv(m: Metric, g: ResultGrid, rows: number[], cols: number[]): void {
    const vis = { rows, cols };
    const csv = toCsv(m, g.info.rows, g.info.cols, (r, c) => this.read(m, g, r, c, vis).reading as Reading | null, { rows, cols });
    const url = URL.createObjectURL(new Blob([csv], { type: "text/csv" }));
    const names = g.domain === "kicks" ? [g.info.kicking, g.info.receiving] : [g.info.offense, g.info.defense || "empty"];
    const a = el("a", { href: url, download: `matrix_${g.domain === "kicks" ? "kicks_" : ""}${names[0]}_vs_${names[1]}_${m.key}.csv` });
    a.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
}
