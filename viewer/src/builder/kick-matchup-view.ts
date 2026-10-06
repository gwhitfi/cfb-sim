// The Matchup tab's kick half: the kicking team's kicks against the receiving
// team's returns and blocks. Same shape as MatchupView.

import type { PlayInfo, TeamInfo } from "../plays";
import { el, load, save } from "./dom";
import {
  groupByType, kickProblem, parseKickForm, planKickRuns, type KickForm,
} from "./kickmatchup";
import { countText, estimate, teamLabel, type HashChoice, type RunPlan } from "./matchup";

const FORM_KEY = "cfbsim.builder.kicks";

export interface KickMatchupHost {
  run(form: KickForm, plan: RunPlan): void;
}

export class KickMatchupView {
  private fresh = load(FORM_KEY) === null;
  private form: KickForm = parseKickForm(load(FORM_KEY));
  private teams: TeamInfo[] = [];
  private running = false;
  private rate: number | null = null;

  private kickingSelect = el("select", { title: "The team that kicks" });
  private receivingSelect = el("select", { title: "The team that returns or blocks" });
  private kickList = el("div", { className: "playlist" });
  private returnList = el("div", { className: "playlist" });
  private summary = el("div", { className: "summary" });
  private kickCount = el("span", { className: "count" });
  private returnCount = el("span", { className: "count" });
  private runButton = el("button", { className: "primary", textContent: "Run kick matrix" });
  private modeSelect = el("select");
  private valueInput = el("input", { type: "number", min: "1", step: "1" });
  private seedInput = el("input", { type: "number", min: "0", step: "1" });
  private hashSelect = el("select");
  private yardInput = el("input", { type: "number", min: "1", max: "99", step: "1", placeholder: "usual" });
  private aggressionInput = el("input", { type: "number", min: "0", max: "100", step: "5" });

  constructor(root: HTMLElement, private host: KickMatchupHost) {
    this.modeSelect.append(
      el("option", { value: "per_cell", textContent: "Runs per cell" }),
      el("option", { value: "total", textContent: "Total runs" }),
    );
    for (const [value, text] of [["rotate", "Rotate"], ["left", "Left"], ["middle", "Middle"], ["right", "Right"]]) {
      this.hashSelect.append(el("option", { value, textContent: text }));
    }
    this.kickingSelect.addEventListener("change", () => {
      this.form.kicking = this.kickingSelect.value;
      this.form.kicks = this.kicksOf().map((p) => p.id);
      this.changed();
    });
    this.receivingSelect.addEventListener("change", () => {
      this.form.receiving = this.receivingSelect.value;
      this.form.returns = this.returnsOf().map((p) => p.id);
      this.changed();
    });
    for (const input of [this.modeSelect, this.valueInput, this.seedInput, this.hashSelect, this.yardInput, this.aggressionInput]) {
      input.addEventListener("input", () => this.read());
    }
    this.runButton.addEventListener("click", () => {
      if (!this.problem()) this.host.run({ ...this.form }, this.plan());
    });

    root.append(
      el("div", { className: "columns" },
        this.side("Kicking team", this.kickCount, this.kickingSelect, this.kickList, true),
        this.side("Receiving team", this.returnCount, this.receivingSelect, this.returnList, false),
      ),
      el("div", { className: "runbar" },
        el("div", { className: "fields" },
          this.field("How many", el("div", { className: "pair" }, this.modeSelect, this.valueInput)),
          this.field("Seed", this.seedInput),
          this.field("Hash", this.hashSelect),
          this.field("Yard line (blank: each kick's usual)", this.yardInput),
          this.field("Return aggression", this.aggressionInput),
        ),
        this.summary,
        this.runButton,
      ),
    );
    this.write();
  }

  setTeams(teams: TeamInfo[]): void {
    this.teams = teams;
    const ids = new Set(teams.map((t) => t.id));
    const first = teams[0]?.id ?? "";
    if (!ids.has(this.form.kicking)) this.form.kicking = first;
    if (!ids.has(this.form.receiving)) this.form.receiving = this.fresh ? (teams[1]?.id ?? first) : first;
    this.fresh = false;
    this.form = this.keepKnown(this.form);
    const options = () => teams.map((t) => el("option", { value: t.id, textContent: teamLabel(teams, t), title: t.id }));
    this.kickingSelect.replaceChildren(...options());
    this.receivingSelect.replaceChildren(...options());
    this.write();
  }

  setRunning(running: boolean): void {
    this.running = running;
    this.refreshSummary();
  }

  setRate(playsPerSecond: number | null): void {
    this.rate = playsPerSecond;
    this.refreshSummary();
  }

  private team(id: string): TeamInfo | undefined {
    return this.teams.find((t) => t.id === id);
  }
  private kicksOf(): PlayInfo[] {
    return this.team(this.form.kicking)?.kicking ?? [];
  }
  private returnsOf(): PlayInfo[] {
    return this.team(this.form.receiving)?.receiving ?? [];
  }
  private chosen(all: PlayInfo[], ids: string[]): PlayInfo[] {
    return all.filter((p) => ids.includes(p.id));
  }

  // Drops saved ids the playbooks no longer have; nothing left means every play.
  private keepKnown(f: KickForm): KickForm {
    const k = this.kicksOf().map((p) => p.id);
    const r = this.returnsOf().map((p) => p.id);
    const kicks = f.kicks.filter((id) => k.includes(id));
    const returns = f.returns.filter((id) => r.includes(id));
    return { ...f, kicks: kicks.length ? kicks : k, returns: returns.length ? returns : r };
  }

  private side(title: string, count: HTMLElement, select: HTMLSelectElement, list: HTMLElement, kicks: boolean): HTMLElement {
    const picks = el("div", { className: "quick" },
      el("button", { textContent: "All", onclick: () => this.setIds(kicks, (all) => all.map((p) => p.id)) }),
      el("button", { textContent: "None", onclick: () => this.setIds(kicks, () => []) }),
    );
    return el("section", { className: "card side" }, el("div", { className: "side-head" }, el("h2", { textContent: title }), count), select, picks, list);
  }

  private setIds(kicks: boolean, pick: (all: PlayInfo[]) => string[]): void {
    const all = kicks ? this.kicksOf() : this.returnsOf();
    if (kicks) this.form.kicks = pick(all);
    else this.form.returns = pick(all);
    this.changed();
  }

  private renderList(list: HTMLElement, plays: PlayInfo[], checked: string[], kicks: boolean): void {
    if (plays.length === 0) {
      list.replaceChildren(el("div", { className: "muted", textContent: "Waiting for the server…" }));
      return;
    }
    const on = new Set(checked);
    const set = (ids: string[]) => {
      if (kicks) this.form.kicks = plays.filter((p) => ids.includes(p.id)).map((p) => p.id);
      else this.form.returns = plays.filter((p) => ids.includes(p.id)).map((p) => p.id);
      this.changed();
    };
    list.replaceChildren(
      ...groupByType(plays).map((g) => {
        const all = g.plays.every((p) => on.has(p.id));
        return el("div", { className: "formation" },
          el("label", { className: "pl-group" },
            el("input", {
              type: "checkbox", checked: all,
              onchange: (e: Event) => {
                const ids = new Set(on);
                for (const p of g.plays) (e.target as HTMLInputElement).checked ? ids.add(p.id) : ids.delete(p.id);
                set([...ids]);
              },
            }),
            el("span", { textContent: g.name }),
          ),
          ...g.plays.map((p) =>
            el("label", { className: "pl-play" },
              el("input", {
                type: "checkbox", checked: on.has(p.id),
                onchange: (e: Event) => {
                  const ids = new Set(on);
                  if ((e.target as HTMLInputElement).checked) ids.add(p.id);
                  else ids.delete(p.id);
                  set([...ids]);
                },
              }),
              el("span", { textContent: p.name }),
            ),
          ),
        );
      }),
    );
  }

  private field(label: string, control: HTMLElement): HTMLElement {
    return el("label", { className: "field" }, el("span", { textContent: label }), control);
  }

  private read(): void {
    const yard = this.yardInput.value.trim();
    this.form = parseKickForm({
      ...this.form,
      mode: this.modeSelect.value,
      value: Number(this.valueInput.value),
      seed: Number(this.seedInput.value),
      hash: this.hashSelect.value as HashChoice,
      yardLine: yard === "" ? null : Number(yard),
      aggression: Number(this.aggressionInput.value),
    });
    this.changed(false);
  }

  private write(): void {
    this.kickingSelect.value = this.form.kicking;
    this.receivingSelect.value = this.form.receiving;
    this.modeSelect.value = this.form.mode;
    this.valueInput.value = String(this.form.value);
    this.seedInput.value = String(this.form.seed);
    this.hashSelect.value = this.form.hash;
    this.yardInput.value = this.form.yardLine === null ? "" : String(this.form.yardLine);
    this.aggressionInput.value = String(this.form.aggression);
    this.renderList(this.kickList, this.kicksOf(), this.form.kicks, true);
    this.renderList(this.returnList, this.returnsOf(), this.form.returns, false);
    this.refreshSummary();
  }

  private changed(rebuild = true): void {
    save(FORM_KEY, this.form);
    if (rebuild) this.write();
    else this.refreshSummary();
  }

  private plan(): RunPlan {
    return planKickRuns(this.chosen(this.kicksOf(), this.form.kicks), this.chosen(this.returnsOf(), this.form.returns), this.form.mode, this.form.value);
  }

  private problem(): string | null {
    return kickProblem(this.form, this.chosen(this.kicksOf(), this.form.kicks), this.chosen(this.returnsOf(), this.form.returns));
  }

  private refreshSummary(): void {
    this.kickCount.textContent = countText(this.form.kicks.length, this.kicksOf().length, "kicks");
    this.returnCount.textContent = countText(this.form.returns.length, this.returnsOf().length, "returns and blocks");
    const plan = this.plan();
    const bad = this.problem();
    const short = this.form.mode === "total" && plan.total !== this.form.value;
    const time = estimate(plan.total, this.rate);
    this.summary.textContent = bad
      ? bad
      : `${plan.cells} cells that run (a return answers only its own kind of kick) × ${plan.perCell} runs = ${plan.total.toLocaleString()} kicks` +
        (short ? ` (${this.form.value.toLocaleString()} asked for)` : "") +
        (time ? `, ${time}` : "");
    this.summary.classList.toggle("bad", !!bad);
    this.runButton.disabled = !!bad || this.running || this.teams.length === 0;
    this.runButton.textContent = this.running ? "Running…" : "Run kick matrix";
  }
}
