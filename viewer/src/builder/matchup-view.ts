// The Matchup tab: pick the teams, check the plays, set the runs, and go.

import type { PlayInfo, TeamInfo } from "../plays";
import { el, load, save } from "./dom";
import {
  estimate,
  groupByFormation,
  parseForm,
  planRuns,
  problem,
  quickPick,
  type HashChoice,
  type MatchupForm,
  type QuickPick,
  type RunPlan,
} from "./matchup";

const FORM_KEY = "cfbsim.builder.matchup";

export interface MatchupHost {
  run(form: MatchupForm, plan: RunPlan): void;
}

export class MatchupView {
  private fresh = load(FORM_KEY) === null; // nothing saved: the first visit
  private form: MatchupForm = parseForm(load(FORM_KEY));
  private teams: TeamInfo[] = [];
  private running = false;
  private rate: number | null = null; // plays per second on the last run

  private offenseSelect = el("select", { title: "The team whose offense runs the plays" });
  private defenseSelect = el("select", { title: "The team whose defense answers" });
  private offenseList = el("div", { className: "playlist" });
  private defenseList = el("div", { className: "playlist" });
  private summary = el("div", { className: "summary" });
  private runButton = el("button", { className: "primary", textContent: "Run matrix" });
  private modeSelect = el("select");
  private valueInput = el("input", { type: "number", min: "1", step: "1" });
  private seedInput = el("input", { type: "number", min: "0", step: "1" });
  private hashSelect = el("select");
  private yardInput = el("input", { type: "number", min: "1", max: "99", step: "1" });
  private huddleInput = el("input", { type: "checkbox" });

  constructor(root: HTMLElement, private host: MatchupHost) {
    this.modeSelect.append(
      el("option", { value: "per_cell", textContent: "Runs per cell" }),
      el("option", { value: "total", textContent: "Total runs" }),
    );
    for (const [value, text] of [["rotate", "Rotate"], ["left", "Left"], ["middle", "Middle"], ["right", "Right"]]) {
      this.hashSelect.append(el("option", { value, textContent: text }));
    }

    this.offenseSelect.addEventListener("change", () => {
      this.form.offense = this.offenseSelect.value;
      this.form.plays = this.offensePlays().map((p) => p.id);
      this.changed();
    });
    this.defenseSelect.addEventListener("change", () => {
      this.form.defense = this.defenseSelect.value;
      this.form.defs = this.defensePlays().map((p) => p.id);
      this.changed();
    });
    this.modeSelect.addEventListener("change", () => this.read());
    for (const input of [this.valueInput, this.seedInput, this.yardInput, this.huddleInput]) {
      input.addEventListener("input", () => this.read());
    }
    this.hashSelect.addEventListener("change", () => this.read());
    this.runButton.addEventListener("click", () => {
      const plan = this.plan();
      if (!this.problem()) this.host.run({ ...this.form }, plan);
    });

    root.append(
      el("div", { className: "columns" },
        this.side("Offense", this.offenseSelect, this.offenseList, true),
        this.side("Defense", this.defenseSelect, this.defenseList, false),
      ),
      el("section", { className: "card runs" },
        el("h2", { textContent: "Runs" }),
        el("div", { className: "fields" },
          this.field("How many", this.modeSelect, this.valueInput),
          this.field("Seed", this.seedInput),
          this.field("Hash", this.hashSelect),
          this.field("Yard line", this.yardInput),
          this.field("No huddle", this.huddleInput, undefined, true),
        ),
        this.summary,
        this.runButton,
      ),
    );
    this.write();
  }

  // The teams and playbooks from the server's hello.
  setTeams(teams: TeamInfo[]): void {
    this.teams = teams;
    const ids = new Set(teams.map((t) => t.id));
    const first = teams[0]?.id ?? "";
    if (!ids.has(this.form.offense)) this.form.offense = first;
    if (this.fresh) {
      this.form.defense = teams[1]?.id ?? first; // a real defense to start with, not the empty field
      this.fresh = false;
    }
    if (this.form.defense && !ids.has(this.form.defense)) this.form.defense = first;
    this.form = this.keepKnown(this.form);
    this.offenseSelect.replaceChildren(...teams.map((t) => el("option", { value: t.id, textContent: `${t.name} (${t.id})` })));
    this.defenseSelect.replaceChildren(
      el("option", { value: "", textContent: "(empty field)" }),
      ...teams.map((t) => el("option", { value: t.id, textContent: `${t.name} (${t.id})` })),
    );
    this.write();
  }

  setRunning(running: boolean): void {
    this.running = running;
    this.refreshSummary();
  }

  // Plays per second measured on a finished run, for the time estimate.
  setRate(playsPerSecond: number | null): void {
    this.rate = playsPerSecond;
    this.refreshSummary();
  }

  // ---- the lists

  private team(id: string): TeamInfo | undefined {
    return this.teams.find((t) => t.id === id);
  }
  private offensePlays(): PlayInfo[] {
    return this.team(this.form.offense)?.offense ?? [];
  }
  private defensePlays(): PlayInfo[] {
    return this.form.defense ? (this.team(this.form.defense)?.defense ?? []) : [];
  }

  // Drops saved play ids the playbooks no longer have; a team with none checked gets every play.
  private keepKnown(f: MatchupForm): MatchupForm {
    const o = this.offensePlays().map((p) => p.id);
    const d = this.defensePlays().map((p) => p.id);
    const plays = f.plays.filter((id) => o.includes(id));
    const defs = f.defs.filter((id) => d.includes(id));
    return { ...f, plays: plays.length ? plays : o, defs: defs.length ? defs : d };
  }

  private side(title: string, select: HTMLSelectElement, list: HTMLElement, offense: boolean): HTMLElement {
    const picks = el("div", { className: "quick" });
    const quick = (text: string, pick: () => QuickPick) =>
      picks.append(el("button", { textContent: text, onclick: () => this.apply(offense, pick()) }));
    quick("All", () => ({ kind: "all" }));
    quick("None", () => ({ kind: "none" }));
    if (offense) {
      quick("Pass", () => ({ kind: "type", type: "pass" }));
      quick("Run", () => ({ kind: "type", type: "run" }));
    }
    return el("section", { className: "card" }, el("h2", { textContent: title }), select, picks, list);
  }

  private apply(offense: boolean, pick: QuickPick): void {
    const plays = offense ? this.offensePlays() : this.defensePlays();
    const ids = quickPick(plays, offense ? this.form.plays : this.form.defs, pick);
    if (offense) this.form.plays = ids;
    else this.form.defs = ids;
    this.changed();
  }

  private renderList(list: HTMLElement, plays: PlayInfo[], checked: string[], offense: boolean, empty: string): void {
    if (plays.length === 0) {
      list.replaceChildren(el("div", { className: "muted", textContent: empty }));
      return;
    }
    const on = new Set(checked);
    list.replaceChildren(
      ...groupByFormation(plays).map((g) => {
        const all = g.plays.every((p) => on.has(p.id));
        const header = el("label", { className: "group" },
          el("input", {
            type: "checkbox",
            checked: all,
            onchange: (e: Event) =>
              this.apply(offense, { kind: "formation", formation: g.formation, on: (e.target as HTMLInputElement).checked }),
          }),
          el("span", { textContent: g.name }),
        );
        return el("div", { className: "formation" }, header,
          ...g.plays.map((p) =>
            el("label", { className: "play", title: p.tags?.join(", ") ?? "" },
              el("input", {
                type: "checkbox",
                checked: on.has(p.id),
                onchange: (e: Event) => {
                  const set = new Set(offense ? this.form.plays : this.form.defs);
                  if ((e.target as HTMLInputElement).checked) set.add(p.id);
                  else set.delete(p.id);
                  const ids = plays.filter((q) => set.has(q.id)).map((q) => q.id);
                  if (offense) this.form.plays = ids;
                  else this.form.defs = ids;
                  this.changed();
                },
              }),
              el("span", { textContent: p.name }),
              el("span", { className: `badge ${p.type}`, textContent: p.type }),
            ),
          ),
        );
      }),
    );
  }

  // ---- the form

  private field(label: string, a: HTMLElement, b?: HTMLElement, inline = false): HTMLElement {
    const controls = b ? [el("div", { className: "pair" }, a, b)] : [a];
    return el("label", { className: inline ? "field inline" : "field" }, el("span", { textContent: label }), ...controls);
  }

  // The form's values from the inputs.
  private read(): void {
    this.form = parseForm({
      ...this.form,
      mode: this.modeSelect.value,
      value: Number(this.valueInput.value),
      seed: Number(this.seedInput.value),
      hash: this.hashSelect.value as HashChoice,
      yardLine: Number(this.yardInput.value),
      huddle: this.huddleInput.checked,
    });
    this.changed(false);
  }

  // Everything out of the form into the inputs and lists.
  private write(): void {
    this.offenseSelect.value = this.form.offense;
    this.defenseSelect.value = this.form.defense;
    this.modeSelect.value = this.form.mode;
    this.valueInput.value = String(this.form.value);
    this.seedInput.value = String(this.form.seed);
    this.hashSelect.value = this.form.hash;
    this.yardInput.value = String(this.form.yardLine);
    this.huddleInput.checked = this.form.huddle;
    this.renderList(this.offenseList, this.offensePlays(), this.form.plays, true, "Waiting for the server…");
    this.renderList(this.defenseList, this.defensePlays(), this.form.defs, false,
      this.form.defense ? "Waiting for the server…" : "An empty field: no defenders, one column.");
    this.refreshSummary();
  }

  private changed(rebuild = true): void {
    save(FORM_KEY, this.form);
    if (rebuild) this.write();
    else this.refreshSummary();
  }

  private plan(): RunPlan {
    const rows = this.form.plays.length;
    const cols = this.form.defense ? this.form.defs.length : 1;
    return planRuns(rows, cols, this.form.mode, this.form.value);
  }

  private problem(): string | null {
    return problem(this.form, {
      offense: new Set(this.offensePlays().map((p) => p.id)),
      defs: new Set(this.defensePlays().map((p) => p.id)),
    });
  }

  private refreshSummary(): void {
    const plan = this.plan();
    const bad = this.problem();
    const rows = this.form.plays.length;
    const cols = this.form.defense ? this.form.defs.length : 1;
    const short = this.form.mode === "total" && plan.total !== this.form.value;
    const time = estimate(plan.total, this.rate);
    this.summary.textContent = bad
      ? bad
      : `${rows} × ${cols} = ${plan.cells} cells × ${plan.perCell} runs = ${plan.total.toLocaleString()} plays` +
        (short ? ` (${this.form.value.toLocaleString()} asked for; each cell gets the same whole number)` : "") +
        (time ? `, ${time}` : "");
    this.summary.classList.toggle("bad", !!bad);
    this.runButton.disabled = !!bad || this.running || this.teams.length === 0;
    this.runButton.textContent = this.running ? "Running…" : "Run matrix";
  }
}
