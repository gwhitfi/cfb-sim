import { describe, expect, it } from "vitest";
import {
  JERSEY_TEXT_WIDTH, NEW_FORM, copyOf, formFromTeam, jerseyLetters, matchesTeam, normalizeHex, saveRequest, slugify, sortProblems, textOn, validate, type TeamDetail, type TeamForm,
} from "./teams";

const good: TeamForm = {
  ...NEW_FORM, id: "my_u", name: "My University", nickname: "Owls", abbr: "MYU",
  offense: "spread_base", defense: "43_base", special: "st_default",
};
const books = { offense: ["spread_base"], defense: ["43_base"], special: ["st_default"] };

describe("validate", () => {
  it("accepts a good new team", () => {
    expect(validate(good, true, books)).toEqual({});
  });
  it("names every field that is wrong, with the server's rules", () => {
    const e = validate({ ...good, id: "Bad Id", name: "", abbr: "TOOLONG1", primary: "red", secondary: "#12", talent: 0, seed: -1, offense: "nope" }, true, books);
    expect(Object.keys(e).sort()).toEqual(["abbr", "colors.primary", "colors.secondary", "id", "name", "playbooks.offense", "roster.seed", "roster.talent"]);
  });
  it("wants a playbook picked, and one that exists", () => {
    expect(validate({ ...good, defense: "" }, true, books)["playbooks.defense"]).toBeDefined();
    expect(validate({ ...good, defense: "gone" }, true, books)["playbooks.defense"]).toBeDefined();
    expect(validate({ ...good, defense: "gone" }, true)["playbooks.defense"]).toBeUndefined(); // list not loaded yet
  });
  it("a new team can't keep a roster; an existing one can, and then talent doesn't matter", () => {
    expect(validate({ ...good, rosterMode: "keep" }, true, books)["roster.mode"]).toBeDefined();
    expect(validate({ ...good, rosterMode: "keep", talent: 0 }, false, books)).toEqual({});
  });
});

describe("saveRequest", () => {
  it("builds the server's message for a new team", () => {
    expect(saveRequest({ ...good, name: " My University ", primary: "#aa00ff" }, true)).toEqual({
      type: "save_team",
      team: {
        id: "my_u", name: "My University", nickname: "Owls", abbr: "MYU",
        colors: { primary: "#AA00FF", secondary: "#F2C230" },
        playbooks: { offense: "spread_base", defense: "43_base", special: "st_default" }, scratch: true,
      },
      roster: { mode: "generate", talent: 50, seed: 1 },
      overwrite: false,
    });
  });
  it("overwrites an existing team, keeping or regenerating its roster", () => {
    expect(saveRequest({ ...good, rosterMode: "keep" }, false)).toMatchObject({ roster: { mode: "keep" }, overwrite: true });
    expect(saveRequest({ ...good, rosterMode: "generate", talent: 80, seed: 9 }, false)).toMatchObject({ roster: { mode: "generate", talent: 80, seed: 9 } });
  });
});

describe("sortProblems", () => {
  it("sends a line to the field it names, and lists the rest apart", () => {
    const r = sortProblems(["abbr: 1-6 letters or digits", "colors.primary: use #RRGGBB", "'my_u' already exists", "teams: not a field"]);
    expect(r.fields).toEqual({ abbr: "1-6 letters or digits", "colors.primary": "use #RRGGBB" });
    expect(r.other).toEqual(["'my_u' already exists", "teams: not a field"]);
  });
});

describe("forms from teams", () => {
  const detail: TeamDetail = {
    id: "fsu", name: "Florida State", nickname: "Seminoles", abbr: "FSU", scratch: false,
    colors: { primary: "#782f40", secondary: "#ffffff" },
    playbooks: { offense: "spread_base", defense: "43_base", special: "st_default" }, roster: { file: "rosters/fsu.json" },
  };
  it("an existing team keeps its roster by default, colors upper-cased", () => {
    expect(formFromTeam(detail)).toMatchObject({ id: "fsu", primary: "#782F40", rosterMode: "keep", scratch: false });
  });
  it("a copy is a new scratch team with a new roster", () => {
    expect(copyOf(formFromTeam(detail))).toMatchObject({ id: "", name: "Florida State copy", scratch: true, rosterMode: "generate", primary: "#782F40" });
  });
});

describe("helpers", () => {
  it("slugifies a name into an id", () => {
    expect(slugify("Furman Paladins!")).toBe("furman_paladins");
    expect(slugify("49ers")).toBe("t_49ers");
    expect(slugify("  ")).toBe("");
    expect(slugify("x".repeat(80))).toHaveLength(40);
  });
  it("normalizes hex colors", () => {
    expect(normalizeHex("#abc")).toBe("#AABBCC");
    expect(normalizeHex("aabbcc")).toBe("#AABBCC");
    expect(normalizeHex(" #12345F ")).toBe("#12345F");
    expect(normalizeHex("#12")).toBeNull();
    expect(normalizeHex("zzzzzz")).toBeNull();
  });
  it("picks readable text for a color", () => {
    expect(textOn("#FFFFFF")).toBe("#000000");
    expect(textOn("#102030")).toBe("#FFFFFF");
    expect(textOn("#F2C230")).toBe("#000000");
  });
});

describe("the team list's search", () => {
  const fsu = { id: "fsu", name: "Florida State", abbr: "FSU", nickname: "Seminoles" };
  it("matches the name, id, abbreviation, or nickname, ignoring case", () => {
    for (const q of ["", "florida", "FSU", "semi", " state "]) expect(matchesTeam(fsu, q)).toBe(true);
    expect(matchesTeam(fsu, "clemson")).toBe(false);
  });
});

describe("the jersey's letters", () => {
  it("fit the shirt: smaller for more letters, squeezed when wide", () => {
    expect(jerseyLetters("ab")).toEqual({ text: "AB", fontSize: 28, squeeze: null });
    expect(jerseyLetters("FSU").squeeze).toBeNull();
    expect(jerseyLetters("WWWW")).toEqual({ text: "WWWW", fontSize: 16, squeeze: JERSEY_TEXT_WIDTH });
    expect(jerseyLetters("toolong").text).toBe("TOOL");
    expect(jerseyLetters("  ").text).toBe("ABC");
  });
});
