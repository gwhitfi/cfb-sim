// The Teams tab's logic, free of the DOM so it can be tested: the form, the
// same checks the server makes (so mistakes show as you type), the save
// message, and where each of the server's complaints belongs.

export type RosterMode = "generate" | "keep";

export interface TeamForm {
  id: string;
  name: string;
  nickname: string;
  abbr: string;
  primary: string; // #RRGGBB
  secondary: string;
  offense: string; // playbook ids
  defense: string;
  special: string;
  scratch: boolean;
  rosterMode: RosterMode;
  talent: number;
  seed: number;
}

export const NEW_FORM: TeamForm = {
  id: "",
  name: "",
  nickname: "",
  abbr: "",
  primary: "#1F4E9C",
  secondary: "#F2C230",
  offense: "",
  defense: "",
  special: "",
  scratch: true,
  rosterMode: "generate",
  talent: 50,
  seed: 1,
};

export type Field =
  | "id" | "name" | "nickname" | "abbr"
  | "colors.primary" | "colors.secondary"
  | "playbooks.offense" | "playbooks.defense" | "playbooks.special"
  | "roster.talent" | "roster.seed" | "roster.mode";

export type FieldErrors = Partial<Record<Field, string>>;

const FIELDS: Field[] = [
  "id", "name", "nickname", "abbr", "colors.primary", "colors.secondary",
  "playbooks.offense", "playbooks.defense", "playbooks.special", "roster.talent", "roster.seed", "roster.mode",
];

// What get_team sends.
export interface TeamDetail {
  id: string;
  name: string;
  nickname: string;
  abbr: string;
  scratch: boolean;
  colors: { primary: string; secondary: string };
  playbooks: { offense: string; defense: string; special: string };
  roster: { file: string; players?: number; by_position?: Record<string, number>; rating?: { overall: number; offense: number; defense: number; special: number } };
}

// An existing team as a form: the roster is kept unless you ask for a new one.
export function formFromTeam(t: TeamDetail): TeamForm {
  return {
    id: t.id, name: t.name, nickname: t.nickname, abbr: t.abbr,
    primary: t.colors.primary.toUpperCase(), secondary: t.colors.secondary.toUpperCase(),
    offense: t.playbooks.offense, defense: t.playbooks.defense, special: t.playbooks.special,
    scratch: t.scratch, rosterMode: "keep", talent: NEW_FORM.talent, seed: NEW_FORM.seed,
  };
}

// A copy to start a new team from: same look and playbooks, no id, a new roster.
export function copyOf(f: TeamForm): TeamForm {
  return { ...f, id: "", name: f.name ? `${f.name} copy` : "", scratch: true, rosterMode: "generate" };
}

// The server's own rules (cfbsim/gen/team_builder.cpp), for instant feedback.
export function validate(f: TeamForm, isNew: boolean, playbooks?: { offense: string[]; defense: string[]; special: string[] }): FieldErrors {
  const e: FieldErrors = {};
  if (!/^[a-z][a-z0-9_]{1,39}$/.test(f.id)) e.id = "2-40 characters: lowercase letters, digits, and _, starting with a letter";
  if (f.name.trim().length < 1 || f.name.length > 60) e.name = "1-60 characters";
  if (f.nickname.length > 40) e.nickname = "At most 40 characters";
  if (!/^[A-Za-z0-9]{1,6}$/.test(f.abbr)) e.abbr = "1-6 letters or digits";
  if (!/^#[0-9A-Fa-f]{6}$/.test(f.primary)) e["colors.primary"] = "Use #RRGGBB";
  if (!/^#[0-9A-Fa-f]{6}$/.test(f.secondary)) e["colors.secondary"] = "Use #RRGGBB";
  for (const side of ["offense", "defense", "special"] as const) {
    const known = playbooks?.[side];
    if (!f[side] || (known && !known.includes(f[side]))) e[`playbooks.${side}`] = `Pick a ${side} playbook`;
  }
  if (isNew && f.rosterMode !== "generate") e["roster.mode"] = "A new team needs a generated roster";
  if (f.rosterMode === "generate") {
    if (!Number.isInteger(f.talent) || f.talent < 1 || f.talent > 100) e["roster.talent"] = "1-100";
    if (!Number.isInteger(f.seed) || f.seed < 0) e["roster.seed"] = "A whole number, 0 or more";
  }
  return e;
}

// The save_team message. An existing team is saved over (overwrite).
export function saveRequest(f: TeamForm, isNew: boolean): Record<string, unknown> {
  return {
    type: "save_team",
    team: {
      id: f.id, name: f.name.trim(), nickname: f.nickname.trim(), abbr: f.abbr,
      colors: { primary: f.primary.toUpperCase(), secondary: f.secondary.toUpperCase() },
      playbooks: { offense: f.offense, defense: f.defense, special: f.special },
      scratch: f.scratch,
    },
    roster: f.rosterMode === "generate" ? { mode: "generate", talent: f.talent, seed: f.seed } : { mode: "keep" },
    overwrite: !isNew,
  };
}

// The server's problems, one per line like "colors.primary: use #RRGGBB": the
// ones that name a field go to it, the rest are listed apart.
export function sortProblems(lines: string[]): { fields: FieldErrors; other: string[] } {
  const fields: FieldErrors = {};
  const other: string[] = [];
  for (const line of lines) {
    const m = /^([a-z.]+): (.*)$/.exec(line);
    const field = m && (FIELDS as string[]).includes(m[1]!) ? (m[1] as Field) : null;
    if (field) fields[field] = m![2]!;
    else other.push(line);
  }
  return { fields, other };
}

// "Furman Paladins!" -> "furman_paladins", for suggesting an id from a name.
export function slugify(name: string): string {
  const slug = name.toLowerCase().replace(/[^a-z0-9]+/g, "_").replace(/^_+|_+$/g, "");
  return (/^[a-z]/.test(slug) ? slug : slug ? `t_${slug}` : "").slice(0, 40);
}

// "#abc", "aabbcc", "#AABBCC" -> "#AABBCC"; null if it isn't a color.
export function normalizeHex(text: string): string | null {
  const t = text.trim().replace(/^#/, "");
  if (/^[0-9a-fA-F]{3}$/.test(t)) return `#${t.split("").map((c) => c + c).join("").toUpperCase()}`;
  if (/^[0-9a-fA-F]{6}$/.test(t)) return `#${t.toUpperCase()}`;
  return null;
}

// Black or white, whichever reads better on `hex`.
export function textOn(hex: string): string {
  const n = normalizeHex(hex);
  if (!n) return "#000000";
  const [r, g, b] = [1, 3, 5].map((i) => parseInt(n.slice(i, i + 2), 16)) as [number, number, number];
  return 0.299 * r + 0.587 * g + 0.114 * b > 150 ? "#000000" : "#FFFFFF";
}

// Does a team match the list's search? Any word of the name, id, abbreviation,
// or nickname, ignoring case; an empty search matches everything.
export function matchesTeam(t: { id: string; name: string; abbr?: string; nickname?: string }, query: string): boolean {
  const q = query.trim().toLowerCase();
  if (!q) return true;
  return [t.name, t.id, t.abbr ?? "", t.nickname ?? ""].some((s) => s.toLowerCase().includes(q));
}

// The jersey preview's letters: up to 4 of the abbreviation, the font size for
// that many, and the width to squeeze them into when they'd run off the
// shirt's body (null: they fit). The body is 56 units wide; the letters get 44.
export const JERSEY_TEXT_WIDTH = 44;
const WIDE = new Set(["M", "W"]);
const NARROW = new Set(["I", "J", "1", "."]);
export function jerseyLetters(abbr: string): { text: string; fontSize: number; squeeze: number | null } {
  const text = (abbr.trim() || "ABC").toUpperCase().slice(0, 4);
  const fontSize = text.length <= 2 ? 28 : text.length === 3 ? 21 : 16;
  // About how wide bold capitals are, in ems.
  const ems = [...text].reduce((n, ch) => n + (WIDE.has(ch) ? 0.95 : NARROW.has(ch) ? 0.34 : 0.68), 0);
  return { text, fontSize, squeeze: ems * fontSize > JERSEY_TEXT_WIDTH ? JERSEY_TEXT_WIDTH : null };
}
