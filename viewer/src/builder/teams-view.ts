// The Teams tab: the teams on the left, and a form to make or edit one on the
// right. Only scratch teams can be changed or deleted (the server enforces it
// too); any team can be copied as the start of a new one.

import type { TeamInfo } from "../plays";
import type { Playbook } from "./connection";
import { el } from "./dom";
import {
  NEW_FORM, copyOf, formFromTeam, normalizeHex, saveRequest, slugify, sortProblems, textOn, validate,
  type Field, type FieldErrors, type RosterMode, type TeamDetail, type TeamForm,
} from "./teams";

export interface TeamsHost {
  select(id: string): void; // ask for the team's details
  save(request: Record<string, unknown>): void;
  remove(id: string): void;
}

export class TeamsView {
  private teams: TeamInfo[] = [];
  private books: Playbook[] = [];
  private form: TeamForm = { ...NEW_FORM };
  private isNew = true;
  private detail: TeamDetail | null = null; // the team being edited
  private selected: string | null = null;
  private touched = new Set<Field>();
  private attempted = false;
  private serverErrors: FieldErrors = {};
  private otherErrors: string[] = [];
  private idAuto = true; // the id follows the name until it's typed in
  private pending = false;
  private afterSave = false; // the reload that follows a save keeps its "Saved" message
  private status = el("div", { className: "muted" });

  private list = el("div", { className: "team-list" });
  private notice = el("div", { className: "notice", hidden: true });
  private preview = el("div", { className: "preview" });
  private errorBox = el("div", { className: "bad errors" });
  private rosterInfo = el("div", { className: "muted roster-info" });
  private errs = new Map<Field, HTMLElement>();
  private inputs: Record<string, HTMLInputElement | HTMLSelectElement> = {};
  private saveButton = el("button", { className: "primary", textContent: "Save team" });
  private copyButton = el("button", { textContent: "Copy as new team", title: "Start a new team from this one's look and playbooks" });
  private deleteButton = el("button", { className: "danger", textContent: "Delete" });
  private rosterModes = el("div", { className: "modes" });
  private scratchNote = el("div", { className: "muted" });

  constructor(root: HTMLElement, private host: TeamsHost) {
    const newButton = el("button", { textContent: "+ New team", className: "primary" });
    newButton.addEventListener("click", () => this.startNew());
    this.saveButton.addEventListener("click", () => this.save());
    this.copyButton.addEventListener("click", () => this.startFrom(copyOf(this.form)));
    this.deleteButton.addEventListener("click", () => {
      if (this.detail && confirm(`Delete ${this.detail.name} (${this.detail.id}) and its roster? This can't be undone.`)) {
        this.pending = true;
        this.host.remove(this.detail.id);
        this.refresh();
      }
    });

    root.append(
      el("div", { className: "teams-layout" },
        el("section", { className: "card" }, el("h2", { textContent: "Teams" }), newButton, this.list),
        el("section", { className: "card team-form" },
          el("h2", { textContent: "Team" }),
          this.notice,
          el("div", { className: "form-grid" },
            this.text("id", "Id", "Letters, digits, and _; can't change once saved"),
            this.text("name", "Name", "Florida State"),
            this.text("nickname", "Nickname", "Seminoles"),
            this.text("abbr", "Abbreviation", "FSU"),
            this.color("colors.primary", "Primary color"),
            this.color("colors.secondary", "Secondary color"),
            this.playbook("playbooks.offense", "Offense playbook", "offense"),
            this.playbook("playbooks.defense", "Defense playbook", "defense"),
            this.playbook("playbooks.special", "Special teams playbook", "special"),
          ),
          this.preview,
          el("h3", { textContent: "Roster" }),
          this.rosterInfo,
          this.rosterModes,
          el("div", { className: "form-grid roster-fields" },
            this.number("roster.talent", "Talent (1-100)", "50 is an average FBS roster"),
            this.seedField(),
          ),
          el("label", { className: "check" }, this.checkbox("scratch"), el("span", { textContent: "Scratch team (expendable)" })),
          this.scratchNote,
          this.errorBox,
          el("div", { className: "actions" }, this.saveButton, this.copyButton, this.deleteButton, this.status),
        ),
      ),
    );
    this.refresh();
  }

  // ---- called by the page

  setTeams(teams: TeamInfo[]): void {
    this.teams = teams;
    if (this.selected && !teams.some((t) => t.id === this.selected)) {
      this.selected = null;
      this.startNew();
    }
    this.drawList();
  }

  setPlaybooks(books: Playbook[]): void {
    this.books = books;
    for (const side of ["offense", "defense", "special"] as const) {
      const select = this.inputs[`playbooks.${side}`] as HTMLSelectElement;
      select.replaceChildren(
        el("option", { value: "", textContent: "Pick one…" }),
        ...books.filter((b) => b.side === side).map((b) => el("option", { value: b.id, textContent: `${b.name} (${b.plays} plays, ${b.scheme})` })),
      );
    }
    if (this.isNew && !this.form.offense) this.form = { ...this.form, ...this.defaultBooks() };
    this.fill();
  }

  // A team's details came back: edit it.
  showTeam(detail: TeamDetail): void {
    if (detail.id !== this.selected) return; // an answer to an earlier click
    this.detail = detail;
    this.isNew = false;
    this.form = formFromTeam(detail);
    const message = this.status.textContent;
    this.resetChecks();
    if (this.afterSave) this.status.textContent = message;
    this.afterSave = false;
    this.fill();
  }

  saved(id: string, created: boolean): void {
    this.pending = false;
    this.status.textContent = created ? `Created ${id}` : `Saved ${id}`;
    this.status.classList.remove("bad");
    this.selected = id;
    this.afterSave = true;
    this.host.select(id);
  }

  deleted(id: string): void {
    this.pending = false;
    if (this.selected === id) this.startNew();
    this.status.textContent = `Deleted ${id}`;
  }

  // The server refused our save or delete: its lines go to the fields they name.
  problems(message: string, lines: string[]): boolean {
    if (!this.pending) return false;
    this.pending = false;
    const { fields, other } = sortProblems(lines.length ? lines : [message]);
    this.serverErrors = fields;
    this.otherErrors = other;
    this.status.textContent = "";
    this.refresh();
    return true;
  }

  // ---- the team list

  private drawList(): void {
    this.list.replaceChildren(
      ...this.teams.map((t) => {
        const c = t.colors;
        const row = el("button", { className: `team-row${t.id === this.selected ? " active" : ""}`, title: t.id },
          el("span", { className: "swatch", style: `background: linear-gradient(90deg, ${c?.primary ?? "#444"} 50%, ${c?.secondary ?? "#888"} 50%)` }),
          el("span", { className: "team-name", textContent: t.name }),
          el("span", { className: "muted", textContent: t.abbr }),
          el("span", { className: `badge ${t.scratch ? "scratch" : "locked"}`, textContent: t.scratch ? "scratch" : "kept", title: t.scratch ? "The Builder can change and delete it" : "The Builder won't change or delete it" }),
        );
        row.addEventListener("click", () => {
          this.selected = t.id;
          this.drawList();
          this.host.select(t.id);
        });
        return row;
      }),
    );
  }

  // ---- the form

  private defaultBooks(): Pick<TeamForm, "offense" | "defense" | "special"> {
    const first = (side: string) => this.books.find((b) => b.side === side)?.id ?? "";
    return { offense: first("offense"), defense: first("defense"), special: first("special") };
  }

  private startNew(): void {
    this.startFrom({ ...NEW_FORM, ...this.defaultBooks(), seed: 1 + Math.floor(Math.random() * 99999) });
  }

  private startFrom(form: TeamForm): void {
    this.selected = null;
    this.detail = null;
    this.isNew = true;
    this.form = form;
    this.idAuto = !form.id;
    this.resetChecks();
    this.drawList();
    this.fill();
  }

  private resetChecks(): void {
    this.touched.clear();
    this.attempted = false;
    this.serverErrors = {};
    this.otherErrors = [];
    this.status.textContent = "";
  }

  private books_of(): { offense: string[]; defense: string[]; special: string[] } | undefined {
    if (!this.books.length) return undefined;
    const ids = (side: string) => this.books.filter((b) => b.side === side).map((b) => b.id);
    return { offense: ids("offense"), defense: ids("defense"), special: ids("special") };
  }

  private locked(): boolean {
    return !this.isNew && !!this.detail && !this.detail.scratch;
  }

  private save(): void {
    this.attempted = true;
    this.serverErrors = {};
    this.otherErrors = [];
    const errors = validate(this.form, this.isNew, this.books_of());
    this.refresh();
    if (Object.keys(errors).length) return;
    this.pending = true;
    this.status.textContent = "Saving…";
    this.host.save(saveRequest(this.form, this.isNew));
    this.refresh();
  }

  // One field's value changed.
  private changed(field: Field, patch: Partial<TeamForm>): void {
    this.form = { ...this.form, ...patch };
    this.touched.add(field);
    delete this.serverErrors[field];
    this.refresh();
  }

  // The form's values into the inputs (a team was loaded or started).
  private fill(): void {
    const f = this.form;
    const set = (key: string, value: string) => {
      const input = this.inputs[key];
      if (input && input.value !== value) input.value = value;
    };
    set("id", f.id);
    set("name", f.name);
    set("nickname", f.nickname);
    set("abbr", f.abbr);
    set("colors.primary", f.primary);
    set("colors.primary.hex", f.primary);
    set("colors.secondary", f.secondary);
    set("colors.secondary.hex", f.secondary);
    set("playbooks.offense", f.offense);
    set("playbooks.defense", f.defense);
    set("playbooks.special", f.special);
    set("roster.talent", String(f.talent));
    set("roster.talent.range", String(f.talent));
    set("roster.seed", String(f.seed));
    (this.inputs.scratch as HTMLInputElement).checked = f.scratch;
    this.refresh();
  }

  // Everything that depends on the form: errors, preview, what's enabled.
  private refresh(): void {
    const f = this.form;
    const local = validate(f, this.isNew, this.books_of());
    const locked = this.locked();
    const all: FieldErrors = { ...local, ...this.serverErrors };
    for (const [field, span] of this.errs) {
      const shown = (this.attempted || this.touched.has(field) || this.serverErrors[field]) && all[field];
      span.textContent = shown || "";
    }
    const lines = [...this.otherErrors];
    this.errorBox.replaceChildren(...lines.map((l) => el("div", { textContent: l })));

    this.notice.hidden = !locked;
    this.notice.textContent = locked ? "This team isn't a scratch team, so the Builder won't change or delete it. You can still copy it as a new team." : "";

    for (const input of Object.values(this.inputs)) input.disabled = locked;
    (this.inputs.id as HTMLInputElement).disabled = locked || !this.isNew;
    const regenerate = f.rosterMode === "generate";
    for (const key of ["roster.talent", "roster.talent.range", "roster.seed"]) this.inputs[key]!.disabled = locked || !regenerate;
    this.seedButtonDisabled(locked || !regenerate);

    this.drawRoster(locked);
    this.drawPreview();

    this.saveButton.disabled = locked || this.pending;
    this.saveButton.textContent = this.isNew ? "Create team" : "Save changes";
    this.copyButton.disabled = this.isNew && !f.name && !this.detail;
    this.deleteButton.hidden = this.isNew;
    this.deleteButton.disabled = locked || this.pending;
    this.scratchNote.textContent = f.scratch
      ? ""
      : "Not scratch: once saved, the Builder won't change or delete this team (you can still copy it).";
  }

  private seedBtn = el("button", { textContent: "New seed", title: "A different random roster" });
  private seedButtonDisabled(disabled: boolean): void {
    this.seedBtn.disabled = disabled;
  }

  private drawRoster(locked: boolean): void {
    const d = this.detail;
    const r = d?.roster;
    this.rosterInfo.textContent = this.isNew
      ? "A new team gets a generated roster: the same talent and seed always make the same players."
      : r?.players !== undefined
        ? `${r.players} players${r.rating ? ` · rating ${r.rating.overall.toFixed(0)} (offense ${r.rating.offense.toFixed(0)}, defense ${r.rating.defense.toFixed(0)}, special ${r.rating.special.toFixed(0)})` : ""}`
        : "";
    this.rosterModes.hidden = this.isNew;
    if (!this.isNew) {
      const mode = (value: RosterMode, label: string) => {
        const input = el("input", { type: "radio", name: "roster-mode", checked: this.form.rosterMode === value, disabled: locked });
        input.addEventListener("change", () => this.changed("roster.mode", { rosterMode: value }));
        return el("label", { className: "check" }, input, el("span", { textContent: label }));
      };
      this.rosterModes.replaceChildren(
        mode("keep", "Keep this roster (changes the look and playbooks only)"),
        mode("generate", "Generate a new roster and depth chart (tactics are kept)"),
      );
    }
  }

  private drawPreview(): void {
    const f = this.form;
    const primary = normalizeHex(f.primary) ?? "#444444";
    const secondary = normalizeHex(f.secondary) ?? "#FFFFFF";
    this.preview.innerHTML = `
      <svg viewBox="0 0 120 124" width="120" height="124" role="img" aria-label="Jersey preview">
        <path d="M30 18 L50 10 Q60 22 70 10 L90 18 L114 42 L98 58 L88 50 L88 112 L32 112 L32 50 L22 58 L6 42 Z"
              fill="${primary}" stroke="${secondary}" stroke-width="3" stroke-linejoin="round"/>
        <path d="M6 42 L22 58 M114 42 L98 58" stroke="${secondary}" stroke-width="7" stroke-linecap="round"/>
        <text x="60" y="78" text-anchor="middle" font-size="26" font-weight="700" font-family="system-ui, sans-serif"
              fill="${secondary}"></text>
      </svg>`;
    const text = this.preview.querySelector("text")!;
    text.textContent = (f.abbr || "ABC").toUpperCase().slice(0, 4);
    text.setAttribute("font-size", String(f.abbr.length <= 2 ? 28 : f.abbr.length === 3 ? 20 : 15)); // the body is narrow
    this.preview.append(
      el("div", {},
        el("strong", { textContent: f.name || "Team name" }),
        el("div", { className: "muted", textContent: f.nickname }),
        el("div", { className: "chips" },
          el("span", { className: "chip", style: `background:${primary};color:${textOn(primary)}`, textContent: primary }),
          el("span", { className: "chip", style: `background:${secondary};color:${textOn(secondary)}`, textContent: secondary }),
        ),
      ),
    );
  }

  // ---- field builders

  private errorFor(field: Field): HTMLElement {
    const span = el("span", { className: "bad field-error" });
    this.errs.set(field, span);
    return span;
  }

  private labelled(text: string, hint: string, control: HTMLElement, field: Field): HTMLElement {
    return el("label", { className: "form-field", title: hint }, el("span", { textContent: text }), control, this.errorFor(field));
  }

  private text(field: "id" | "name" | "nickname" | "abbr", label: string, hint: string): HTMLElement {
    const input = el("input", { type: "text", placeholder: hint, autocomplete: "off" });
    this.inputs[field] = input;
    input.addEventListener("input", () => {
      if (field === "id") {
        this.idAuto = input.value === "";
        this.changed("id", { id: input.value });
      } else if (field === "name") {
        const patch: Partial<TeamForm> = { name: input.value };
        if (this.isNew && this.idAuto) {
          patch.id = slugify(input.value);
          (this.inputs.id as HTMLInputElement).value = patch.id;
        }
        this.changed("name", patch);
      } else if (field === "abbr") {
        input.value = input.value.toUpperCase();
        this.changed("abbr", { abbr: input.value });
      } else {
        this.changed("nickname", { nickname: input.value });
      }
    });
    return this.labelled(label, hint, input, field);
  }

  private color(field: "colors.primary" | "colors.secondary", label: string): HTMLElement {
    const key = field === "colors.primary" ? "primary" : "secondary";
    const picker = el("input", { type: "color", className: "picker" });
    const hex = el("input", { type: "text", className: "hex", maxLength: 7, spellcheck: false });
    this.inputs[field] = picker;
    this.inputs[`${field}.hex`] = hex;
    picker.addEventListener("input", () => {
      hex.value = picker.value.toUpperCase();
      this.changed(field, { [key]: picker.value.toUpperCase() });
    });
    hex.addEventListener("input", () => {
      const n = normalizeHex(hex.value);
      if (n) picker.value = n.toLowerCase();
      this.changed(field, { [key]: n ?? hex.value });
    });
    return this.labelled(label, "Pick a color, or type a hex code", el("span", { className: "color-pair" }, picker, hex), field);
  }

  private playbook(field: "playbooks.offense" | "playbooks.defense" | "playbooks.special", label: string, side: "offense" | "defense" | "special"): HTMLElement {
    const select = el("select");
    this.inputs[field] = select;
    select.addEventListener("change", () => this.changed(field, { [side]: select.value }));
    return this.labelled(label, `Only ${side} playbooks are listed`, select, field);
  }

  private number(field: "roster.talent", label: string, hint: string): HTMLElement {
    const num = el("input", { type: "number", min: "1", max: "100", step: "1", className: "num" });
    const range = el("input", { type: "range", min: "1", max: "100", step: "1" });
    this.inputs[field] = num;
    this.inputs[`${field}.range`] = range;
    num.addEventListener("input", () => {
      range.value = num.value;
      this.changed(field, { talent: Number(num.value) });
    });
    range.addEventListener("input", () => {
      num.value = range.value;
      this.changed(field, { talent: Number(range.value) });
    });
    return this.labelled(label, hint, el("span", { className: "color-pair" }, num, range), field);
  }

  private seedField(): HTMLElement {
    const input = el("input", { type: "number", min: "0", step: "1", className: "num" });
    this.inputs["roster.seed"] = input;
    input.addEventListener("input", () => this.changed("roster.seed", { seed: Number(input.value) }));
    this.seedBtn.addEventListener("click", (e) => {
      e.preventDefault();
      input.value = String(1 + Math.floor(Math.random() * 99999));
      this.changed("roster.seed", { seed: Number(input.value) });
    });
    return this.labelled("Seed", "The same talent and seed make the same roster", el("span", { className: "color-pair" }, input, this.seedBtn), "roster.seed");
  }

  private checkbox(key: "scratch"): HTMLInputElement {
    const box = el("input", { type: "checkbox" });
    this.inputs[key] = box;
    box.addEventListener("change", () => {
      this.form = { ...this.form, scratch: box.checked };
      this.refresh();
    });
    return box;
  }
}
