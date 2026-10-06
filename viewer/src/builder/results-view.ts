// The Results tab: every result of a run on one page, filling in as cells
// arrive. A toolbar (the run's title and settings, Table or Matrix, the metric
// chips, the filters, what to color against, export, saved runs); the table
// (a row per offensive play, a column per shown metric, each play opening into
// its defenses, the All rows at the bottom) or the matrix (plays against
// defenses for one metric); and a drawer with a cell's detail and Watch
// buttons; and the Sets panel, where a run is saved to the browser or a saved
// one (the browser's, or the server's) reopened or compared. Comparing puts a
// saved run (A) beside the one on screen (B): each metric's A, B, and Δ. The rows, filters, and colors
// come from table.ts, the sets from sets.ts; this file only draws.

import {
  cannotCompare, compare, compareCsv, deltaAdvantage, deltaSpan, readDelta, settingsDiff, sharedShown, sortCompared,
  type CompareSide, type Comparison, type Delta,
} from "./compare";
import { detailSections } from "./detail";
import { el, load, save } from "./dom";
import { advantage, heatColor, type Baseline, type BaselineMode } from "./heat";
import { nextDir, sortIndices, toCsv, type SortDir } from "./order";
import {
  ResultGrid, appliesTo, countOf, formatDelta, formatValue, halfWidth, isNoisy, metricByKey, settingsText, titleOf, totalKind,
  type Domain, type Metric, type PlayKind, type Sample,
} from "./results";
import { describeSaved, type SavedRun } from "./saved";
import { STORAGE_BUDGET, SetStore, browserStorage, defaultName, formatSize, shortDate, type SetMeta } from "./sets";
import {
  NO_FILTERS, baselines, defenseRows, filterGroups, hiddenCount, metricsShown, playRows, presetsFor, readRow, setShown, shown,
  sortRows, tableCsv, totalRows, type Filters, type Shown, type SortBy, type TableRow,
} from "./table";

const PREFS_KEY = "cfbsim.builder.results.v2";

type View = "table" | "matrix";

// Remembered across runs and reloads.
interface Prefs {
  view: View;
  chips: Partial<Record<Domain, string[]>>; // the metrics on, per kind of run (missing: all)
  matrixMetric: Record<Domain, string>; // the metric the matrix shows
  baseline: BaselineMode;
}

function loadPrefs(): Prefs {
  const r = (load(PREFS_KEY) ?? {}) as Partial<Prefs>;
  const keys = (v: unknown) => (Array.isArray(v) ? v.filter((x): x is string => typeof x === "string") : undefined);
  return {
    view: r.view === "matrix" ? "matrix" : "table",
    chips: { scrimmage: keys(r.chips?.scrimmage), kicks: keys(r.chips?.kicks) },
    matrixMetric: {
      scrimmage: metricByKey(r.matrixMetric?.scrimmage ?? "", "scrimmage").key,
      kicks: metricByKey(r.matrixMetric?.kicks ?? "", "kicks").key,
    },
    baseline: r.baseline === "targets" ? "targets" : "average",
  };
}

export interface ResultsHost {
  watch(grid: ResultGrid, sample: Sample, row: number, col: number): void; // replay a play of the run on screen
  openSaved(file: string): void;
  compareSaved(file: string): void; // load a server run, then compareWith() it
}

// The matrix view's sort: the rows by a column (or the All column), or the columns by a row.
interface MatrixSort {
  axis: "rows" | "cols";
  by: number | "total";
  dir: SortDir;
}

const KIND_LABELS: Record<PlayKind, string> = { pass: "Pass", run: "Run", kickoff: "Kickoffs", punt: "Punts", field_goal: "Field goals" };

export class ResultsView {
  private grid: ResultGrid | null = null;
  private source = "";
  private names = new Map<string, string>();
  private prefs = loadPrefs();
  private filters: Filters = NO_FILTERS;
  private tableSort: (SortBy & { side?: CompareSide }) | null = null; // side: when comparing, A, B, or Δ
  private against: { grid: ResultGrid; name: string } | null = null; // A, when comparing
  private matrixSort: MatrixSort | null = null;
  private expanded = new Set<number>(); // grid rows opened into their defenses
  private selected: { r: number; c: number } | null = null;
  private menu: "plays" | "defs" | null = null; // the open filter menu
  private saved: SavedRun[] = [];
  private head = el("div", { className: "results-head" });
  private body = el("div", { className: "results-body" });
  private drawer = el("aside", { className: "drawer" });
  // The Sets panel: drawn on its own (not on every cell), so a name being typed survives a live run.
  private sets = new SetStore(browserStorage());
  private setsPanel = el("div", { className: "sets-panel" });
  private setsOpen = false;
  private setName = "";
  private setsMessage: { text: string; bad: boolean } | null = null;
  private renaming: string | null = null;
  private frame = 0;

  constructor(root: HTMLElement, private host: ResultsHost) {
    root.append(this.head, this.body, this.drawer, this.setsPanel);
    document.addEventListener("keydown", (e) => {
      if (e.key !== "Escape" || root.hidden) return;
      if (this.setsOpen) return this.showSets(false);
      if (this.menu) this.menu = null;
      else if (this.selected) this.selected = null;
      else return;
      this.draw();
    });
    // A click outside an open filter menu closes it.
    document.addEventListener("mousedown", (e) => {
      const target = e.target as HTMLElement;
      if (this.setsOpen && !target.closest?.(".sets-panel, .sets-button")) this.showSets(false);
      if (this.menu && !target.closest?.(".menu")) {
        this.menu = null;
        this.draw();
      }
    });
    this.draw();
    this.drawSets();
  }

  // A new run (null: none yet). `source`: where it came from ("live run", a saved file).
  setGrid(grid: ResultGrid | null, source = ""): void {
    this.grid = grid;
    this.source = source;
    this.filters = NO_FILTERS;
    this.tableSort = null;
    this.matrixSort = null;
    this.expanded.clear();
    this.selected = null;
    this.menu = null;
    // A comparison carries over to the next run (rerun, then compare), unless it can't.
    if (this.against && (!grid || cannotCompare(this.against.grid, grid))) this.against = null;
    this.draw();
    this.drawSets();
  }

  // Compares the run on screen (B) with `grid` (A). The reason it can't, or null.
  compareWith(grid: ResultGrid, name: string): string | null {
    if (!this.grid) return "Open or run something to compare it with first.";
    const why = cannotCompare(grid, this.grid);
    if (why) return why;
    this.against = { grid, name };
    this.tableSort = null;
    this.matrixSort = null;
    this.selected = null;
    this.setsOpen = false;
    this.draw();
    this.drawSets();
    return null;
  }

  private endComparison(): void {
    this.against = null;
    this.tableSort = null;
    this.matrixSort = null;
    this.draw();
  }

  private comparison(): Comparison | null {
    return this.grid && this.against ? compare(this.against.grid, this.grid) : null;
  }

  setSaved(runs: SavedRun[]): void {
    this.saved = runs;
    this.drawSets();
  }

  // Team names for the title (ids until they're known).
  setTeams(teams: { id: string; name: string }[]): void {
    this.names = new Map(teams.map((t) => [t.id, t.name]));
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

  private setPrefs(patch: Partial<Prefs>): void {
    this.prefs = { ...this.prefs, ...patch };
    save(PREFS_KEY, this.prefs);
    this.draw();
  }

  private setFilters(f: Filters): void {
    this.filters = f;
    this.draw();
  }

  private shownMetrics(domain: Domain): Metric[] {
    return metricsShown(domain, this.prefs.chips[domain]);
  }

  // The matrix's metric: the remembered one if it's on, else the first that is.
  private matrixMetric(domain: Domain): Metric | null {
    const on = this.shownMetrics(domain);
    return on.find((m) => m.key === this.prefs.matrixMetric[domain]) ?? on[0] ?? null;
  }

  // ---- drawing

  private draw(): void {
    const g = this.grid;
    if (!g) {
      this.head.replaceChildren(el("div", { className: "results-tools" }, el("span", { className: "spacer" }), this.setsButton()));
      this.body.replaceChildren(el("p", { className: "muted", textContent: "No results yet. Set up a matchup and run it, or open a saved run." }));
      this.drawDrawer();
      return;
    }
    const cmp = this.comparison();
    const vis = cmp ? sharedShown(cmp, shown(g.info, this.filters)) : shown(g.info, this.filters);
    const domain = g.domain;
    const metrics = this.shownMetrics(domain);
    const view = this.prefs.view;

    const name = (id: string) => this.names.get(id) ?? id;
    const progress = g.done < g.total ? ` · ${g.done}/${g.total} cells` : "";
    this.head.replaceChildren(
      el("div", { className: "results-title" },
        el("strong", { textContent: titleOf(g.info, name) }),
        el("span", { className: "muted", textContent: settingsText(g.info, g.settings) + progress }),
        ...(this.source ? [el("span", { className: "source", textContent: this.source })] : []),
      ),
      ...(cmp ? [this.compareBar()] : []),
      el("div", { className: "results-tools" },
        this.segmented<View>([["table", "Table"], ["matrix", "Matrix"]], view, (v) => this.setPrefs({ view: v })),
        ...this.kindToggles(g),
        this.filterMenu(g, "plays"),
        this.filterMenu(g, "defs"),
        ...(hiddenCount(this.filters) > 0
          ? [el("button", { className: "link", textContent: "Show all", title: "Clear the filters", onclick: () => this.setFilters(NO_FILTERS) })]
          : []),
        el("span", { className: "spacer" }),
        ...(cmp ? [] : [this.select("Color against", [["average", "Run average"], ["targets", "Targets"]], this.prefs.baseline, (v) => this.setPrefs({ baseline: v as BaselineMode }))]),
        el("button", { textContent: "Export CSV", title: "What's shown, laid out as on screen", onclick: () => this.exportCsv(g, vis) }),
        this.setsButton(),
      ),
      this.chips(domain),
    );

    if (vis.rows.length === 0 || vis.cols.length === 0) {
      this.body.replaceChildren(el("p", {
        className: "muted",
        textContent: cmp ? "Nothing to compare: the runs share no shown plays and defenses." : "The filters hide everything.",
      }));
    } else if (metrics.length === 0) {
      this.body.replaceChildren(el("p", { className: "muted", textContent: "No metrics are on. Pick some above." }));
    } else if (cmp) {
      const m = this.matrixMetric(domain)!;
      this.body.replaceChildren(
        ...this.settingsNote(cmp),
        this.legend(domain, "Δ is B − A · faded: within the noise (might be luck)"),
        view === "table" ? this.compareTable(cmp, metrics, vis) : this.compareMatrix(cmp, m, vis),
        ...this.onlyNote(cmp),
      );
    } else if (view === "table") {
      this.body.replaceChildren(this.legend(domain, ""), this.table(g, metrics, vis));
    } else {
      const m = this.matrixMetric(domain)!;
      const base = baselines(g, m, vis, this.prefs.baseline, "cell");
      this.body.replaceChildren(this.legend(domain, this.baselineText(m, base)), this.matrix(g, m, vis, base));
    }
    this.drawDrawer();
  }

  private segmented<T extends string>(options: [T, string][], value: T, onPick: (v: T) => void): HTMLElement {
    return el("div", { className: "segmented" },
      ...options.map(([v, text]) => el("button", { textContent: text, className: v === value ? "active" : "", onclick: () => onPick(v) })));
  }

  private select(label: string, options: string[][], value: string, onChange: (v: string) => void): HTMLElement {
    const s = el("select", {}, ...options.map(([v, text]) => el("option", { value: v!, textContent: text!, selected: v === value })));
    s.addEventListener("change", () => onChange(s.value));
    return el("label", { className: "inline" }, label, s);
  }

  private setsButton(): HTMLElement {
    return el("button", {
      className: `sets-button${this.setsOpen ? " open" : ""}`, textContent: "Sets ▾",
      title: "Save this run, or open a saved one",
      onclick: () => this.showSets(!this.setsOpen),
    });
  }

  private legend(domain: Domain, text: string): HTMLElement {
    return el("div", { className: "legend" },
      el("span", { className: "muted", textContent: domain === "kicks" ? "returning team" : "defense" }),
      el("span", { className: "ramp" }),
      el("span", { className: "muted", textContent: domain === "kicks" ? "kicking team" : "offense" }),
      ...(text ? [el("span", { className: "muted", textContent: text })] : []),
    );
  }

  private baselineText(m: Metric, b: Map<PlayKind, Baseline | null>): string {
    const parts: string[] = [];
    for (const [kind, base] of b) {
      if (!base) continue;
      const where = b.size > 1 ? `${KIND_LABELS[kind].toLowerCase()} rows ` : "";
      const what = base.source === "target" ? `target${base.note ? ` (${base.note})` : ""}` : this.prefs.baseline === "targets" ? "run average (no target)" : "run average";
      parts.push(`${where}centered on ${formatValue(m, base.center)}: ${what}`);
    }
    return parts.join(" · ");
  }

  // ---- the toolbar's filters and chips

  // A toggle per kind of play (only when the run has more than one).
  private kindToggles(g: ResultGrid): HTMLElement[] {
    const kinds = g.kinds();
    if (kinds.length < 2) return [];
    return [el("div", { className: "segmented", title: "Show or hide a kind of play" },
      ...kinds.map((k) => {
        const on = !this.filters.kinds.includes(k);
        return el("button", {
          textContent: KIND_LABELS[k], className: on ? "active" : "",
          onclick: () => this.setFilters({ ...this.filters, kinds: on ? [...this.filters.kinds, k] : this.filters.kinds.filter((x) => x !== k) }),
        });
      }))];
  }

  // The Plays (or Defenses) checkbox menu, grouped by formation (kicks: by kind).
  private filterMenu(g: ResultGrid, side: "plays" | "defs"): HTMLElement {
    const kicks = g.domain === "kicks";
    const label = side === "plays" ? (kicks ? "Kicks" : "Plays") : kicks ? "Returns" : "Defenses";
    const total = side === "plays" ? g.info.rows.length : g.info.cols.length;
    const hidden = this.filters[side];
    const ids = (side === "plays" ? g.info.rows : g.info.cols).map((p) => p.id);
    const count = ids.filter((id) => !hidden.includes(id)).length;
    const set = (list: string[], on: boolean) => this.setFilters({ ...this.filters, [side]: setShown(hidden, list, on) });

    const open = this.menu === side;
    const button = el("button", {
      className: `menu-button${count < total ? " filtered" : ""}`,
      textContent: `${label} ${count < total ? `${count} of ${total}` : "(all)"} ▾`,
      onclick: () => { this.menu = open ? null : side; this.draw(); },
    });
    const wrap = el("div", { className: "menu" }, button);
    if (!open) return wrap;

    const checkbox = (checked: boolean, onChange: (on: boolean) => void, indeterminate = false) => {
      const c = el("input", { type: "checkbox", checked, indeterminate });
      c.addEventListener("change", () => onChange(c.checked));
      return c;
    };
    const panel = el("div", { className: "menu-panel" },
      el("div", { className: "menu-quick" },
        el("button", { textContent: "All", onclick: () => set(ids, true) }),
        el("button", { textContent: "None", onclick: () => set(ids, false) }),
      ),
    );
    for (const group of filterGroups(g.info, side)) {
      const groupIds = group.items.map((i) => i.id);
      const on = groupIds.filter((id) => !hidden.includes(id)).length;
      panel.append(
        el("label", { className: "menu-group" },
          checkbox(on === groupIds.length, (c) => set(groupIds, c), on > 0 && on < groupIds.length), group.name),
        ...group.items.map((item) =>
          el("label", { className: "menu-item" }, checkbox(!hidden.includes(item.id), (c) => set([item.id], c)), item.name)),
      );
    }
    wrap.append(panel);
    return wrap;
  }

  // The metric chips: the presets, then one chip per metric.
  private chips(domain: Domain): HTMLElement {
    const on = new Set(this.shownMetrics(domain).map((m) => m.key));
    const setChips = (keys: string[]) => this.setPrefs({ chips: { ...this.prefs.chips, [domain]: keys } });
    const all = presetsFor(domain)[0]!.keys;
    const view = this.prefs.view;
    const matrixKey = this.matrixMetric(domain)?.key;
    return el("div", { className: "metric-chips" },
      el("span", { className: "muted", textContent: "Metrics" }),
      ...presetsFor(domain).map((p) => el("button", {
        className: "preset", textContent: p.name, title: `Show ${p.keys.length} metrics`,
        onclick: () => setChips(p.keys),
      })),
      el("span", { className: "divider" }),
      ...all.map((key) => {
        const m = metricByKey(key, domain);
        const isOn = on.has(key);
        const isMatrix = view === "matrix" && key === matrixKey;
        return el("button", {
          className: ["mchip", isOn ? "on" : "", isMatrix ? "current" : ""].filter(Boolean).join(" "),
          textContent: m.label,
          title: view === "matrix" ? "Click to show in the matrix; shift-click to turn on or off" : "Click to turn on or off",
          onclick: (e: MouseEvent) => {
            if (view === "matrix" && isOn && !e.shiftKey) {
              this.setPrefs({ matrixMetric: { ...this.prefs.matrixMetric, [domain]: key } });
            } else if (view === "matrix" && !isOn && !e.shiftKey) {
              // Turning a metric on in the matrix view also shows it.
              this.setPrefs({ chips: { ...this.prefs.chips, [domain]: all.filter((k) => on.has(k) || k === key) }, matrixMetric: { ...this.prefs.matrixMetric, [domain]: key } });
            } else {
              setChips(all.filter((k) => (k === key ? !isOn : on.has(k))));
            }
          },
        });
      }),
    );
  }

  // ---- the table

  // Every row in the order shown: each play (its defenses under it when opened), then the All rows.
  private tableRows(g: ResultGrid, vis: Shown): TableRow[] {
    const sortMetric = this.tableSort ? metricByKey(this.tableSort.metric, g.domain) : null;
    const dir = this.tableSort?.dir ?? null;
    const cmp = this.comparison();
    const sorted = (rows: TableRow[]) =>
      cmp ? sortCompared(cmp, rows, sortMetric, this.tableSort?.side ?? "d", dir) : sortRows(g, rows, sortMetric, dir);
    const out: TableRow[] = [];
    for (const row of sorted(playRows(g, vis))) {
      out.push(row);
      if (this.expanded.has(row.row!)) out.push(...sorted(defenseRows(g, row.row!, vis)));
    }
    return [...out, ...totalRows(g, vis)];
  }

  private table(g: ResultGrid, metrics: Metric[], vis: Shown): HTMLElement {
    const kicks = g.domain === "kicks";
    const playBase = new Map(metrics.map((m) => [m.key, baselines(g, m, vis, this.prefs.baseline, "play")]));
    const cellBase = new Map(metrics.map((m) => [m.key, baselines(g, m, vis, this.prefs.baseline, "cell")]));

    const sortBy = (key: string) => {
      const same = this.tableSort?.metric === key;
      const dir = nextDir(same ? this.tableSort!.dir : null);
      this.tableSort = dir ? { metric: key, dir } : null;
      this.draw();
    };
    const arrow = (key: string) => (this.tableSort?.metric === key ? (this.tableSort.dir === "desc" ? " ▼" : " ▲") : "");

    const head = el("tr", {},
      el("th", { className: "corner", textContent: kicks ? "Kick" : "Play" }),
      ...metrics.map((m) => el("th", {
        className: "sortable",
        title: `${m.label} · click to sort${this.baselineText(m, playBase.get(m.key)!) ? ` · ${this.baselineText(m, playBase.get(m.key)!)}` : ""}`,
        onclick: () => sortBy(m.key),
      }, m.label + arrow(m.key))),
    );

    const body = el("tbody");
    let firstTotal = true;
    for (const row of this.tableRows(g, vis)) {
      let head: HTMLElement;
      const cls: string[] = [`row-${row.level}`];
      if (row.level === "play") {
        const r = row.row!;
        const open = this.expanded.has(r);
        const p = g.info.rows[r]!;
        head = el("th", {
          className: "rowhead toggle", title: `${p.id} · click to ${open ? "close" : "open"} its ${kicks ? "returns" : "defenses"}`,
          onclick: () => { open ? this.expanded.delete(r) : this.expanded.add(r); this.draw(); },
        }, el("span", { className: "twisty", textContent: open ? "▾" : "▸" }), p.name, el("span", { className: `badge ${p.type}`, textContent: p.type.replace("_", " ") }));
        if (open) cls.push("open");
      } else if (row.level === "def") {
        head = el("th", { className: "rowhead sub", title: g.info.cols[row.col!]!.id, textContent: row.label });
      } else {
        head = el("th", { className: "rowhead", textContent: row.label });
        if (firstTotal) cls.push("first-total");
        firstTotal = false;
      }
      body.append(el("tr", { className: cls.join(" ") }, head, ...metrics.map((m) => {
        const kind = row.kind === "both" ? totalKind(m) : row.kind;
        const bases = row.level === "def" ? cellBase.get(m.key)! : playBase.get(m.key)!;
        const base = row.level !== "total" && kind !== "both" ? bases.get(kind) ?? null : null;
        return this.cell(g, m, row, base);
      })));
    }
    return el("div", { className: "table-wrap" }, el("table", { className: "matrix results-table" }, el("thead", {}, head), body));
  }

  // A row's reading for a metric, colored against `base` (null: no color).
  private cell(g: ResultGrid, m: Metric, row: TableRow, base: Baseline | null): HTMLElement {
    const { stats, reading: rd } = readRow(g, m, row);
    const total = row.level === "total";
    const cls = total ? "total" : "";
    if (!stats) return el("td", { className: `pending ${cls}`.trim(), textContent: "…" });
    const cellPick = row.level === "def" ? { r: row.row!, c: row.col! } : null;
    if (cellPick && g.cells[cellPick.r]?.[cellPick.c]?.applies === false) {
      return el("td", { className: "na", textContent: "·", title: `${g.info.cols[cellPick.c]?.name} doesn't answer ${g.info.rows[cellPick.r]?.name}, so nothing ran` });
    }
    const n = countOf(stats);
    const unit = g.domain === "kicks" ? "kicks" : "plays";
    if (!rd) return el("td", { className: cls, textContent: "–", title: `Means nothing here · ${n} ${unit}` });

    const hw = halfWidth(rd);
    const noisy = isNoisy(m, rd);
    const td = el("td", { className: [cls, noisy ? "noisy" : "", cellPick ? "pick" : ""].filter(Boolean).join(" ") });
    td.title = `${n} ${unit}` + (hw !== null ? ` · ±${formatValue({ ...m, digits: m.percent ? 1 : 2 }, hw)} (95%)` : "") + (noisy ? " · too few plays to trust" : "");
    td.textContent = formatValue(m, rd.value);
    // A thin cell gets a fainter color: it may be luck.
    if (base) td.style.background = heatColor(advantage(m, base, rd.value) * (noisy ? 0.5 : 1));
    if (cellPick) this.pickable(td, cellPick.r, cellPick.c);
    return td;
  }

  private pickable(td: HTMLElement, r: number, c: number): void {
    if (this.selected?.r === r && this.selected.c === c) td.classList.add("selected");
    td.addEventListener("click", () => {
      this.selected = { r, c };
      this.draw();
    });
  }

  // ---- the matrix

  // The rows and columns shown, in the matrix's sorted order.
  private matrixOrder(g: ResultGrid, m: Metric, vis: Shown): Shown {
    const s = this.matrixSort;
    if (!s) return vis;
    const value = (r: number, c: number) => this.matrixRead(g, m, r, c, vis)?.value ?? null;
    if (s.axis === "rows") {
      const idx = sortIndices(vis.rows.length, (i) => value(vis.rows[i]!, s.by === "total" ? -1 : s.by), s.dir);
      return { rows: idx.map((i) => vis.rows[i]!), cols: vis.cols };
    }
    const idx = sortIndices(vis.cols.length, (i) => value(s.by === "total" ? -1 : s.by, vis.cols[i]!), s.dir);
    return { rows: vis.rows, cols: idx.map((i) => vis.cols[i]!) };
  }

  // The matrix cell at (r, c) as a table row; r or c of -1 is the All of that column, row, or both.
  private matrixRow(g: ResultGrid, r: number, c: number, vis: Shown): TableRow {
    const rows = r >= 0 ? [r] : vis.rows;
    const cols = c >= 0 ? [c] : vis.cols;
    const level = r >= 0 && c >= 0 ? "def" : "total";
    return { key: `m:${r}:${c}`, label: "", level, kind: r >= 0 ? g.rowKind(r) : "both", rows, cols, ...(r >= 0 ? { row: r } : {}), ...(c >= 0 && r >= 0 ? { col: c } : {}) };
  }

  // A matrix cell's reading; when comparing, its Δ (what the matrix shows, sorts by, and exports).
  private matrixRead(g: ResultGrid, m: Metric, r: number, c: number, vis: Shown) {
    const cmp = this.comparison();
    if (!cmp) return readRow(g, m, this.matrixRow(g, r, c, vis)).reading;
    const d = readDelta(cmp, m, this.matrixRow(g, r, c, vis)).d;
    return d === null ? null : { value: d, se: null };
  }

  private matrix(g: ResultGrid, m: Metric, vis: Shown, base: Map<PlayKind, Baseline | null>): HTMLElement {
    const { rows, cols } = this.matrixOrder(g, m, vis);
    const arrow = (axis: MatrixSort["axis"], by: number | "total") =>
      this.matrixSort && this.matrixSort.axis === axis && this.matrixSort.by === by ? (this.matrixSort.dir === "desc" ? " ▼" : " ▲") : "";
    const sortBy = (axis: MatrixSort["axis"], by: number | "total") => {
      const same = this.matrixSort && this.matrixSort.axis === axis && this.matrixSort.by === by;
      const dir = nextDir(same ? this.matrixSort!.dir : null);
      this.matrixSort = dir ? { axis, by, dir } : null;
      this.draw();
    };

    const head = el("tr", {},
      el("th", { className: "corner", textContent: m.label }),
      ...cols.map((c) => el("th", {
        className: "sortable", title: `${g.info.cols[c]!.id} · click to sort the rows by this column`,
        onclick: () => sortBy("rows", c),
      }, g.info.cols[c]!.name + arrow("rows", c))),
      el("th", { className: "total sortable", title: "Sort the rows by their totals", onclick: () => sortBy("rows", "total") }, "All" + arrow("rows", "total")),
    );

    const body = el("tbody");
    for (const r of rows) {
      const p = g.info.rows[r]!;
      const kind = g.rowKind(r);
      body.append(el("tr", {},
        el("th", { className: "rowhead sortable", title: `${p.id} · click to sort the columns by this row`, onclick: () => sortBy("cols", r) },
          p.name + arrow("cols", r), el("span", { className: `badge ${p.type}`, textContent: p.type.replace("_", " ") })),
        ...cols.map((c) => this.cell(g, m, this.matrixRow(g, r, c, vis), base.get(kind) ?? null)),
        this.cell(g, m, this.matrixRow(g, r, -1, vis), null),
      ));
    }
    body.append(el("tr", { className: "first-total" },
      el("th", { className: "rowhead sortable", title: "Sort the columns by their totals", onclick: () => sortBy("cols", "total") }, "All" + arrow("cols", "total")),
      ...cols.map((c) => this.cell(g, m, this.matrixRow(g, -1, c, vis), null)),
      this.cell(g, m, this.matrixRow(g, -1, -1, vis), null),
    ));
    return el("div", { className: "table-wrap" }, el("table", { className: "matrix" }, el("thead", {}, head), body));
  }

  // ---- comparing

  private compareBar(): HTMLElement {
    return el("div", { className: "compare-bar" },
      el("span", {}, el("strong", { textContent: "A " }), this.against!.name),
      el("span", { className: "muted", textContent: "beside" }),
      el("span", {}, el("strong", { textContent: "B " }), this.source || "the run on screen"),
      el("button", { className: "link", textContent: "End comparison", onclick: () => this.endComparison() }),
    );
  }

  // The settings the runs differ in, above the table.
  private settingsNote(cmp: Comparison): HTMLElement[] {
    const diffs = settingsDiff(cmp.a, cmp.b);
    if (!diffs.length) return [];
    const notSame = diffs.some((d) => !d.sameTest);
    return [el("div", { className: `compare-note${notSame ? " warn" : ""}` },
      el("strong", { textContent: "Settings that differ: " }),
      diffs.map((d) => (d.a.includes(d.label) ? `${d.a} → ${d.b}` : `${d.label} ${d.a} → ${d.b}`)).join(" · "),
      ...(notSame ? [el("span", { textContent: " · these runs didn't test the same thing" })] : []),
    )];
  }

  // What only one run has, below the table.
  private onlyNote(cmp: Comparison): HTMLElement[] {
    const kicks = cmp.b.domain === "kicks";
    const part = (side: "A" | "B", o: { plays: string[]; defs: string[] }) => {
      const bits = [
        ...(o.plays.length ? [`${kicks ? "kicks" : "plays"}: ${o.plays.join(", ")}`] : []),
        ...(o.defs.length ? [`${kicks ? "returns" : "defenses"}: ${o.defs.join(", ")}`] : []),
      ];
      return bits.length ? [el("p", { className: "muted" }, el("strong", { textContent: `Only in ${side} ` }), `(left out): ${bits.join("; ")}`)] : [];
    };
    return [...part("A", cmp.onlyA), ...part("B", cmp.onlyB)];
  }

  // A Δ cell: B − A, colored toward the side it favors; faded when it might be luck.
  private deltaCell(m: Metric, x: Delta, span: number, opts: { total?: boolean; pending?: boolean } = {}): HTMLElement {
    if (opts.pending) return el("td", { className: "pending delta", textContent: "…" });
    const td = el("td", { className: ["delta", opts.total ? "total" : "", x.withinNoise ? "within" : ""].filter(Boolean).join(" ") });
    td.textContent = formatDelta(m, x.d);
    if (x.d === null) return td;
    td.title = `A ${formatValue(m, x.a!.value)} · B ${formatValue(m, x.b!.value)} · Δ ${formatDelta(m, x.d)}` +
      (x.noise !== null ? ` ± ${formatDelta(m, x.noise).replace("+", "")} (95%)` : " (its spread isn't known)") +
      (x.withinNoise ? " · within the noise: might be luck" : "");
    if (!opts.total) td.style.background = heatColor(deltaAdvantage(m, x.d, span) * (x.withinNoise ? 0.3 : 1));
    return td;
  }

  private compareTable(cmp: Comparison, metrics: Metric[], vis: Shown): HTMLElement {
    const g = cmp.b;
    const kicks = g.domain === "kicks";
    const rows = this.tableRows(g, vis);
    const read = new Map<string, Delta>();
    for (const row of rows) for (const m of metrics) read.set(`${row.key}|${m.key}`, readDelta(cmp, m, row));
    // Each column's full color: the widest Δ among its play rows (defense rows: among theirs).
    const span = (m: Metric, level: TableRow["level"]) =>
      deltaSpan(rows.filter((r) => r.level === level).map((r) => read.get(`${r.key}|${m.key}`)!));

    const sortBy = (key: string, side: CompareSide) => {
      const same = this.tableSort?.metric === key && (this.tableSort.side ?? "d") === side;
      const dir = nextDir(same ? this.tableSort!.dir : null);
      this.tableSort = dir ? { metric: key, side, dir } : null;
      this.draw();
    };
    const arrow = (key: string, side: CompareSide) =>
      this.tableSort?.metric === key && (this.tableSort.side ?? "d") === side ? (this.tableSort.dir === "desc" ? " ▼" : " ▲") : "";

    const top = el("tr", {},
      el("th", { className: "corner", rowSpan: 2, textContent: kicks ? "Kick" : "Play" }),
      ...metrics.map((m) => el("th", { className: "cmp-group", colSpan: 3, title: m.label, textContent: m.label })));
    const sub = el("tr", {}, ...metrics.flatMap((m) => (["a", "b", "d"] as const).map((side) => el("th", {
      className: `sortable sub-head${side === "a" ? " cmp-group" : ""}`,
      title: `Sort by ${side === "d" ? "Δ (B − A)" : side.toUpperCase()}`,
      onclick: () => sortBy(m.key, side),
    }, (side === "d" ? "Δ" : side.toUpperCase()) + arrow(m.key, side)))));

    const body = el("tbody");
    let firstTotal = true;
    for (const row of rows) {
      const cls = [`row-${row.level}`];
      let head: HTMLElement;
      if (row.level === "play") {
        const r = row.row!;
        const open = this.expanded.has(r);
        const p = g.info.rows[r]!;
        head = el("th", { className: "rowhead toggle", onclick: () => { open ? this.expanded.delete(r) : this.expanded.add(r); this.draw(); } },
          el("span", { className: "twisty", textContent: open ? "▾" : "▸" }), p.name, el("span", { className: `badge ${p.type}`, textContent: p.type.replace("_", " ") }));
        if (open) cls.push("open");
      } else if (row.level === "def") {
        head = el("th", { className: "rowhead sub", textContent: row.label });
      } else {
        head = el("th", { className: "rowhead", textContent: row.label });
        if (firstTotal) cls.push("first-total");
        firstTotal = false;
      }
      const tr = el("tr", { className: cls.join(" ") }, head);
      for (const m of metrics) {
        const x = read.get(`${row.key}|${m.key}`)!;
        const pending = readRow(g, m, row).stats === null;
        const total = row.level === "total";
        const plain = (rd: Delta["a"], first: boolean) =>
          el("td", {
            className: [first ? "cmp-group" : "", total ? "total" : "", isNoisy(m, rd) ? "noisy" : ""].filter(Boolean).join(" "),
            textContent: pending && !first ? "…" : rd ? formatValue(m, rd.value) : "–",
          });
        const cells = [plain(x.a, true), plain(x.b, false), this.deltaCell(m, x, span(m, row.level), { total, pending })];
        if (row.level === "def") for (const td of cells) this.pickable(td, row.row!, row.col!), td.classList.add("pick");
        tr.append(...cells);
      }
      body.append(tr);
    }
    return el("div", { className: "table-wrap" }, el("table", { className: "matrix results-table compare-table" }, el("thead", {}, top, sub), body));
  }

  private compareMatrix(cmp: Comparison, m: Metric, vis: Shown): HTMLElement {
    const g = cmp.b;
    const { rows, cols } = this.matrixOrder(g, m, vis);
    const span = deltaSpan(rows.flatMap((r) => cols.map((c) => readDelta(cmp, m, this.matrixRow(g, r, c, vis)))));
    const arrow = (axis: MatrixSort["axis"], by: number | "total") =>
      this.matrixSort && this.matrixSort.axis === axis && this.matrixSort.by === by ? (this.matrixSort.dir === "desc" ? " ▼" : " ▲") : "";
    const sortBy = (axis: MatrixSort["axis"], by: number | "total") => {
      const same = this.matrixSort && this.matrixSort.axis === axis && this.matrixSort.by === by;
      const dir = nextDir(same ? this.matrixSort!.dir : null);
      this.matrixSort = dir ? { axis, by, dir } : null;
      this.draw();
    };
    const cell = (r: number, c: number) => {
      const row = this.matrixRow(g, r, c, vis);
      const td = this.deltaCell(m, readDelta(cmp, m, row), span, { total: r < 0 || c < 0, pending: readRow(g, m, row).stats === null });
      if (r >= 0 && c >= 0) {
        td.classList.add("pick");
        this.pickable(td, r, c);
      }
      return td;
    };
    const head = el("tr", {},
      el("th", { className: "corner", textContent: `Δ ${m.label}` }),
      ...cols.map((c) => el("th", { className: "sortable", onclick: () => sortBy("rows", c) }, g.info.cols[c]!.name + arrow("rows", c))),
      el("th", { className: "total sortable", onclick: () => sortBy("rows", "total") }, "All" + arrow("rows", "total")),
    );
    const body = el("tbody");
    for (const r of rows) {
      const p = g.info.rows[r]!;
      body.append(el("tr", {},
        el("th", { className: "rowhead sortable", onclick: () => sortBy("cols", r) },
          p.name + arrow("cols", r), el("span", { className: `badge ${p.type}`, textContent: p.type.replace("_", " ") })),
        ...cols.map((c) => cell(r, c)),
        cell(r, -1),
      ));
    }
    body.append(el("tr", { className: "first-total" },
      el("th", { className: "rowhead sortable", onclick: () => sortBy("cols", "total") }, "All" + arrow("cols", "total")),
      ...cols.map((c) => cell(-1, c)),
      cell(-1, -1),
    ));
    return el("div", { className: "table-wrap" }, el("table", { className: "matrix" }, el("thead", {}, head), body));
  }

  // The drawer's comparison: each metric's A, B, and Δ for the cell, and A's plays to watch.
  private compareDetail(cmp: Comparison, r: number, c: number): HTMLElement[] {
    const g = cmp.b;
    const kind = g.rowKind(r);
    const row: TableRow = { key: "", label: "", level: "def", kind, rows: [r], cols: [c], row: r, col: c };
    const ar = cmp.rowInA.get(r), ac = cmp.colInA.get(c);
    const metrics = (g.domain === "kicks" ? this.shownMetrics("kicks") : this.shownMetrics("scrimmage")).filter((m) => appliesTo(m, kind));
    const out: HTMLElement[] = [
      el("h3", { textContent: `A (${this.against!.name}) and B` }),
      el("table", { className: "kv compare-kv" }, el("tbody", {},
        el("tr", {}, el("td"), el("td", { className: "muted", textContent: "A" }), el("td", { className: "muted", textContent: "B" }), el("td", { className: "muted", textContent: "Δ" })),
        ...metrics.map((m) => {
          const x = readDelta(cmp, m, row);
          return el("tr", { className: x.withinNoise ? "within" : "" },
            el("td", { textContent: m.label }),
            el("td", { textContent: x.a ? formatValue(m, x.a.value) : "–" }),
            el("td", { textContent: x.b ? formatValue(m, x.b.value) : "–" }),
            el("td", { textContent: formatDelta(m, x.d), title: x.noise !== null ? `± ${formatDelta(m, x.noise).replace("+", "")} (95%)` : "" }));
        }))),
    ];
    const samples = ar !== undefined && ac !== undefined ? cmp.a.cells[ar]?.[ac]?.samples ?? [] : [];
    if (samples.length) {
      out.push(el("div", { className: "watch" },
        el("span", { className: "muted", textContent: "Watch A's" }),
        ...samples.map((s) => el("button", {
          textContent: `${s.kind} (${s.yards.toFixed(1)} yd)`,
          onclick: () => this.host.watch(cmp.a, s, ar!, ac!),
        }))));
    }
    out.push(el("h3", { textContent: "B in full" }));
    return out;
  }

  // ---- the drawer

  private drawDrawer(): void {
    const g = this.grid;
    const sel = this.selected;
    this.drawer.classList.toggle("open", !!(g && sel));
    if (!g || !sel) {
      this.drawer.replaceChildren();
      return;
    }
    const { r, c } = sel;
    const cell = g.cells[r]?.[c];
    const row = g.info.rows[r]!, col = g.info.cols[c]!;
    this.drawer.replaceChildren(
      el("div", { className: "detail-head" },
        el("strong", { textContent: `${row.name} vs ${col.name}` }),
        el("button", { textContent: "×", title: "Close (Esc)", onclick: () => { this.selected = null; this.draw(); } }),
      ),
    );
    if (!cell) {
      this.drawer.append(el("p", { className: "muted", textContent: "This cell hasn't arrived yet." }));
      return;
    }
    const cmp = this.comparison();
    if (cmp) this.drawer.append(...this.compareDetail(cmp, r, c));
    const samples = cell.samples ?? [];
    if (samples.length) {
      this.drawer.append(el("div", { className: "watch" },
        el("span", { className: "muted", textContent: cmp ? "Watch B's" : "Watch the" }),
        ...samples.map((s) => el("button", {
          textContent: `${s.kind} (${s.yards.toFixed(1)} yd)`,
          title: `Seed ${s.seed}, ${s.hash} hash${s.flip ? ", flipped" : ""}: opens the play in the viewer`,
          onclick: () => this.host.watch(g, s, r, c),
        })),
      ));
    }
    for (const s of detailSections(cell.stats, g.rowKind(r), g.domain)) {
      this.drawer.append(el("h3", { textContent: s.title }),
        el("table", { className: "kv" }, el("tbody", {}, ...s.rows.map(([k, v]) => el("tr", {}, el("td", { textContent: k }), el("td", { textContent: v }))))));
    }
  }

  // ---- the Sets panel

  private showSets(open: boolean): void {
    this.setsOpen = open;
    this.renaming = null;
    this.setsMessage = null;
    if (open && this.grid) this.setName = defaultName(this.grid.info, (id) => this.names.get(id) ?? id, new Date());
    this.head.querySelector(".sets-button")?.classList.toggle("open", open);
    this.drawSets();
    if (open) this.setsPanel.querySelector<HTMLInputElement>("input.set-name")?.select();
  }

  private openSet(meta: SetMeta): void {
    const run = this.sets.read(meta.id);
    if (!run) {
      this.setsMessage = { text: `"${meta.name}" can't be read (saved by another version of the page?).`, bad: true };
      return this.drawSets();
    }
    this.setsOpen = false;
    this.setGrid(this.gridOf(run), `set "${meta.name}"`);
  }

  private compareWithSet(meta: SetMeta): void {
    const run = this.sets.read(meta.id);
    const why = run ? this.compareWith(this.gridOf(run), `set "${meta.name}"`) : `"${meta.name}" can't be read.`;
    if (why) {
      this.setsMessage = { text: why, bad: true };
      this.drawSets();
    }
  }

  private gridOf(run: { info: ResultGrid["info"]; settings: ResultGrid["settings"]; cells: NonNullable<ResultGrid["cells"][number][number]>[] }): ResultGrid {
    const grid = new ResultGrid(run.info, run.settings);
    for (const cell of run.cells) grid.add(cell);
    return grid;
  }

  private drawSets(): void {
    const p = this.setsPanel;
    p.classList.toggle("open", this.setsOpen);
    if (!this.setsOpen) return p.replaceChildren();
    const g = this.grid;

    // Save the run on screen.
    const name = el("input", { className: "set-name", value: this.setName, placeholder: "Name", disabled: !g });
    const doSave = () => {
      if (!this.grid) return;
      const r = this.sets.save(this.setName, this.grid, titleOf(this.grid.info, (id) => this.names.get(id) ?? id));
      this.setsMessage = r.ok ? { text: `Saved "${r.meta.name}".`, bad: false } : { text: r.reason, bad: true };
      this.drawSets();
    };
    name.addEventListener("input", () => (this.setName = name.value));
    name.addEventListener("keydown", (e) => e.key === "Enter" && doSave());
    const partial = g && g.done < g.total ? `The run isn't complete: this keeps the ${g.done} of ${g.total} cells there are.` : "";
    p.replaceChildren(
      el("h3", { textContent: "Save this run" }),
      el("div", { className: "set-save" }, name, el("button", { className: "primary", textContent: "Save set", disabled: !g, onclick: doSave })),
      ...(g ? [] : [el("p", { className: "muted", textContent: "Run a matchup or open a run to save it." })]),
      ...(partial ? [el("p", { className: "muted", textContent: partial })] : []),
      ...(this.setsMessage ? [el("p", { className: this.setsMessage.bad ? "bad" : "muted", textContent: this.setsMessage.text })] : []),
    );

    // The browser's sets.
    const sets = this.sets.list();
    p.append(el("h3", {},
      "In this browser",
      el("span", { className: "muted", textContent: ` · ${formatSize(this.sets.used())} of about ${formatSize(STORAGE_BUDGET)}` })));
    if (!sets.length) p.append(el("p", { className: "muted", textContent: "No sets saved yet." }));
    for (const s of sets) p.append(this.setRow(s));

    // The server's saved runs.
    p.append(el("h3", { textContent: "On the server (out/matrix/)" }));
    if (!this.saved.length) p.append(el("p", { className: "muted", textContent: "None (or not connected)." }));
    for (const r of this.saved) {
      p.append(el("div", { className: "set-row" },
        el("div", { className: "set-what" }, el("span", { textContent: describeSaved(r) })),
        el("div", { className: "set-actions" },
          el("button", { textContent: "Open", onclick: () => { this.showSets(false); this.host.openSaved(r.file); } }),
          el("button", {
            textContent: "Compare", disabled: !this.grid, title: "Compare the run on screen (B) with this one (A)",
            onclick: () => { this.showSets(false); this.host.compareSaved(r.file); },
          })),
      ));
    }
  }

  private setRow(s: SetMeta): HTMLElement {
    const size = `${s.rows}×${s.cols}, ${s.runs} ${s.kind === "kicks" ? "kicks" : "runs"}`;
    const part = s.cells < s.rows * s.cols ? ` · part: ${s.cells} of ${s.rows * s.cols} cells` : "";
    const details = el("span", { className: "muted", textContent: `${s.title} · ${size} · ${shortDate(new Date(s.saved))} · ${formatSize(s.chars)}${part}` });
    if (this.renaming === s.id) {
      const input = el("input", { value: s.name });
      const done = (keep: boolean) => {
        if (keep && !this.sets.rename(s.id, input.value)) return;
        this.renaming = null;
        this.drawSets();
      };
      input.addEventListener("keydown", (e) => (e.key === "Enter" ? done(true) : e.key === "Escape" && (e.stopPropagation(), done(false))));
      queueMicrotask(() => input.select());
      return el("div", { className: "set-row" },
        el("div", { className: "set-what" }, input, details),
        el("div", { className: "set-actions" },
          el("button", { textContent: "Rename", onclick: () => done(true) }),
          el("button", { textContent: "Cancel", onclick: () => done(false) })));
    }
    return el("div", { className: "set-row" },
      el("div", { className: "set-what" }, el("a", { className: "set-open", textContent: s.name, title: "Open", onclick: () => this.openSet(s) }), details),
      el("div", { className: "set-actions" },
        el("button", { textContent: "Open", onclick: () => this.openSet(s) }),
        el("button", { textContent: "Compare", disabled: !this.grid, title: "Compare the run on screen (B) with this set (A)", onclick: () => this.compareWithSet(s) }),
        el("button", { textContent: "Rename", onclick: () => { this.renaming = s.id; this.drawSets(); } }),
        el("button", {
          className: "danger", textContent: "Delete",
          onclick: () => {
            if (!confirm(`Delete the set "${s.name}"? This can't be undone.`)) return;
            this.sets.remove(s.id);
            this.drawSets();
          },
        })));
  }

  // ---- export

  private exportCsv(g: ResultGrid, vis: Shown): void {
    const domain = g.domain;
    let csv: string;
    let what: string;
    const cmp = this.comparison();
    if (this.prefs.view === "table") {
      csv = cmp ? compareCsv(cmp, this.shownMetrics(domain), this.tableRows(g, vis)) : tableCsv(g, this.shownMetrics(domain), this.tableRows(g, vis));
      what = cmp ? "compare" : "table";
    } else {
      const m = this.matrixMetric(domain);
      if (!m) return;
      const order = this.matrixOrder(g, m, vis);
      csv = toCsv(m, g.info.rows, g.info.cols, (r, c) => this.matrixRead(g, m, r, c, vis), order);
      what = cmp ? `${m.key}_delta` : m.key;
    }
    const url = URL.createObjectURL(new Blob([csv], { type: "text/csv" }));
    const names = domain === "kicks" ? [g.info.kicking, g.info.receiving] : [g.info.offense, g.info.defense || "empty"];
    const a = el("a", { href: url, download: `results_${domain === "kicks" ? "kicks_" : ""}${names[0]}_vs_${names[1]}_${what}.csv` });
    a.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
}
