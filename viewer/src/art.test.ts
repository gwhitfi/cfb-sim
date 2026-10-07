import { describe, expect, it } from "vitest";
import { cutAt, formationSvg, playSvg, readArtMessage, PlaybookArtStore, type DefensivePlaybookArt, type FormationArt, type PlayArt, type PlaybookArt, type Pt } from "./art";

const art: PlaybookArt = { team: "clemson_2025", playbook: "spread_base", formations: [], plays: [] };

describe("the playbook art message", () => {
  it("gives the side and its art", () => {
    expect(readArtMessage({ type: "playbook_art", team: "home", art })).toEqual({ side: "home", unit: "offense", art, error: "" });
  });

  it("gives the side and the error when there's no art", () => {
    expect(readArtMessage({ type: "playbook_art", team: "away", error: "unknown team 'x'" })).toEqual({
      side: "away",
      unit: "offense",
      art: null,
      error: "unknown team 'x'",
    });
    expect(readArtMessage({ type: "playbook_art", team: "away", art: { team: "x" } })?.art).toBeNull();
  });

  it("ignores anything else", () => {
    expect(readArtMessage({ type: "started" })).toBeNull();
    expect(readArtMessage({ type: "playbook_art", team: "visitors", art })).toBeNull();
    expect(readArtMessage(null)).toBeNull();
  });
});

const defense: DefensivePlaybookArt = {
  unit: "defense", team: "clemson_2025", playbook: "43_base",
  formations: [{ id: "43_over", name: "4-3 Over", package: "base", players: [{ slot: "FS", role: "s", x: 0, y: 13 }] }],
  plays: [{ id: "cover_3", name: "Cover 3", formation: "43_over", coverage: "cover_3",
    players: [{ slot: "FS", role: "s", x: 0, y: 13 }],
    jobs: [{ slot: "FS", kind: "zone", points: [[0, 13], [0, 15]],
      area: { shape: "box", drop: [0, 15], width: 18, depth: [10, 45] } }] }],
};

describe("art units and independent storage", () => {
  it("decodes explicit offense and defensive geometry without changing either payload", () => {
    expect(readArtMessage({ type: "playbook_art", team: "home", unit: "offense", art })).toEqual({
      side: "home", unit: "offense", art, error: "",
    });
    expect(readArtMessage({ type: "playbook_art", team: "away", unit: "defense", art: defense })).toEqual({
      side: "away", unit: "defense", art: defense, error: "",
    });
  });

  it("rejects unknown units and mismatched payloads, including defense in a legacy envelope", () => {
    expect(readArtMessage({ type: "playbook_art", team: "home", unit: "kicking", art })).toBeNull();
    expect(readArtMessage({ type: "playbook_art", team: "home", unit: null, art })).toBeNull();
    for (const msg of [
      { unit: "defense", art }, { unit: "offense", art: defense }, { art: defense },
      { unit: "defense", art: { ...defense, plays: null } },
      { unit: "offense", art: { ...art, playbook: 123 } },
    ]) {
      const parsed = readArtMessage({ type: "playbook_art", team: "home", ...msg });
      expect(parsed?.art).toBeNull();
      expect(parsed?.error).toMatch(/invalid/);
    }
  });

  it("keeps home/away and offense/defense separate even for the same team", () => {
    const store = new PlaybookArtStore();
    for (const side of ["home", "away"] as const) {
      store.receive({ type: "playbook_art", team: side, art }); // legacy offense
      store.receive({ type: "playbook_art", team: side, unit: "defense", art: defense });
    }
    expect(store.home).toEqual({ offense: art, defense });
    expect(store.away).toEqual({ offense: art, defense });
    const changed = { ...defense, playbook: "another_book" };
    store.receive({ type: "playbook_art", team: "away", unit: "defense", art: changed });
    expect(store.home.defense).toBe(defense);
    expect(store.away.defense).toBe(changed);
    expect(store.away.offense).toBe(art);
  });

  it("clears only the failed entry and can recover on a later valid message", () => {
    const store = new PlaybookArtStore();
    for (const side of ["home", "away"] as const) {
      store.receive({ type: "playbook_art", team: side, art });
      store.receive({ type: "playbook_art", team: side, unit: "defense", art: defense });
    }
    expect(store.receive({ type: "playbook_art", team: "home", unit: "defense", error: "bad layout" })).toEqual({
      side: "home", unit: "defense", art: null, error: "bad layout",
    });
    expect(store.home).toEqual({ offense: art });
    expect(store.away).toEqual({ offense: art, defense });
    store.receive({ type: "playbook_art", team: "home", unit: "defense", art: defense });
    store.receive({ type: "playbook_art", team: "away", unit: "offense", art: {} });
    expect(store.home).toEqual({ offense: art, defense });
    expect(store.away).toEqual({ defense });
    store.receive({ type: "playbook_art", team: "away", unit: "unknown", art });
    expect(store.away).toEqual({ defense });
    store.clear();
    expect(store.home).toEqual({});
    expect(store.away).toEqual({});
  });
});

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

const trips: FormationArt = {
  id: "trips",
  name: "Trips <Rt>",
  personnel: "11",
  players: [
    { slot: "LT", role: "ol", x: -3, y: -1 },
    { slot: "C", role: "ol", x: 0, y: -1 },
    { slot: "RT", role: "ol", x: 3, y: -1 },
    { slot: "X", role: "wr", x: -14.67, y: -1 },
    { slot: "H", role: "wr", x: 9.67, y: -2 },
    { slot: "QB", role: "qb", x: 0, y: -5 },
    { slot: "RB", role: "back", x: 2, y: -5 },
  ],
};

const curl: PlayArt = {
  id: "all_curl",
  name: "All Curl",
  formation: "trips",
  type: "pass",
  flip: true,
  routes: [
    { slot: "X", points: [[-14.67, -1], [-14.67, 11], [-13.67, 9]], end: "stop", primary: false },
    { slot: "H", points: [[9.67, -2], [9.67, 2], [1.67, 10], [1.67, 8]], end: "stop", primary: true },
    { slot: "RB", points: [[2, -5], [4, -6], [10, -5], [12, -2]], end: "continue", primary: false },
  ],
  qb: [[0, -5], [0, -6.3]],
  path: null,
  pulls: [],
};

// Every number pair in the polylines with this exact class, as [x, y].
function lines(svg: string, cls: string): Pt[][] {
  const out: Pt[][] = [];
  for (const m of svg.matchAll(new RegExp(`<polyline class="${cls}" points="([^"]*)"`, "g"))) {
    out.push(m[1]!.split(" ").map((p) => p.split(",").map(Number) as Pt));
  }
  return out;
}

describe("drawing a card", () => {
  it("shows sideline to sideline, 8 yd behind the line to 18 past it", () => {
    expect(formationSvg(trips)).toContain('viewBox="-26.67 -18 53.33 26"');
  });

  it("draws a formation as its players, letters on the skill players only", () => {
    const s = formationSvg(trips);
    expect(s.match(/<circle class="art-player/g)).toHaveLength(7);
    expect(s).toContain(">X</text>");
    expect(s).toContain(">RB</text>");
    expect(s).not.toContain(">LT</text>");
    expect(s).not.toContain(">QB</text>");
    expect(s).not.toContain("art-route");
    expect(s).toContain('aria-label="Trips &lt;Rt&gt;"');
  });

  it("draws the first read's route as the primary, and only that one", () => {
    const s = playSvg(curl, trips);
    expect(lines(s, "art-route primary")).toEqual([[[9.67, 2], [9.67, -2], [1.67, -10], [1.67, -8]]]);
    expect(lines(s, "art-route")).toHaveLength(2);
    expect(lines(s, "art-drop")).toEqual([[[0, 5], [0, 6.3]]]);
  });

  it("mirrors a flipped play: every x negated", () => {
    const s = playSvg(curl, trips);
    const f = playSvg(curl, trips, { flip: true });
    const mirror = (ls: Pt[][]) => ls.map((l) => l.map(([x, y]) => [x === 0 ? 0 : -x, y]));
    expect(lines(f, "art-route")).toEqual(mirror(lines(s, "art-route")));
    expect(lines(f, "art-route primary")).toEqual(mirror(lines(s, "art-route primary")));
    expect(f).toContain('cx="14.67"'); // X
    // A play that can't flip is drawn as it is.
    expect(playSvg({ ...curl, flip: false }, trips, { flip: true })).toBe(s);
  });

  it("cuts a route at the top of the card and ends it in an arrow", () => {
    const go: PlayArt = { ...curl, routes: [{ slot: "X", points: [[-14.67, -1], [-14.67, 39]], end: "continue", primary: true }] };
    const [route] = lines(playSvg(go, trips), "art-route primary");
    expect(route).toEqual([[-14.67, 1], [-14.67, -17.4]]);
    const sit: PlayArt = { ...curl, routes: [{ slot: "X", points: [[-14.67, -1], [-14.67, 25], [-14.67, 22]], end: "stop", primary: true }] };
    const cut = playSvg(sit, trips);
    expect(cut).toContain('<polyline class="art-route primary art-head"'); // an arrow, not a stop bar
    expect(cut).not.toContain('<line class="art-route primary art-head"');
  });

  it("draws a run's path as the primary, with the pullers", () => {
    const run: PlayArt = {
      ...curl,
      type: "run",
      routes: [],
      qb: [],
      path: [[2, -5], [0.5, -4.3], [2.25, -1]],
      pulls: [{ slot: "LG", points: [[-1.5, -1], [-1.5, -2.2], [5, -2.2], [5, 0.5]] }],
    };
    const s = playSvg(run, trips);
    expect(lines(s, "art-path primary")).toEqual([[[2, 5], [0.5, 4.3], [2.25, 1]]]);
    expect(lines(s, "art-pull")).toHaveLength(1);
    expect(s).not.toContain("art-drop");
  });
});

describe("cutting a line", () => {
  it("keeps a line under the top, and cuts one that passes it", () => {
    expect(cutAt([[0, 0], [0, 10]], 17)).toEqual({ points: [[0, 0], [0, 10]], cut: false });
    expect(cutAt([[0, 0], [10, 20], [0, 30]], 10)).toEqual({ points: [[0, 0], [5, 10]], cut: true });
  });
});
