import { describe, expect, it } from "vitest";
import { defensiveFormationSvg, defensivePlaySvg, type DefensivePlayArt, type DefensiveFormationArt, type ArtZone } from "./art";

const formation: DefensiveFormationArt = {
  id: "base", name: 'Base <front> "A"', package: "base",
  players: [
    { slot: "SDE", role: "dl", x: 4.5, y: 1 },
    { slot: "DT", role: "dl", x: 2, y: 1 },
    { slot: "NT", role: "dl", x: -1.25, y: 1 },
    { slot: "WDE", role: "dl", x: -5, y: 1 },
    { slot: "SAM", role: "lb", x: 5.4, y: 5 },
    { slot: "MIKE", role: "lb", x: 0, y: 5.5 },
    { slot: "WILL", role: "lb", x: -6.5, y: 5 },
    { slot: "SCB", role: "cb", x: 14.67, y: 4.5 },
    { slot: "WCB", role: "cb", x: -14.67, y: 4.5 },
    { slot: "SS", role: "s", x: 6.67, y: 13 },
    { slot: "FS", role: "s", x: -6.5, y: 13 },
  ],
};
const call: DefensivePlayArt = {
  id: "base_call", name: "Base Call", formation: formation.id, coverage: "cover_3",
  players: formation.players, jobs: [],
};
function zoneSvg(area: ArtZone): string {
  return defensivePlaySvg({ ...call, jobs: [{ slot: "FS", kind: "zone", points: [[0, 13], area.drop], area }] });
}

describe("defensive base card rendering", () => {
  it("shows just the defensive formation and all slot labels in the fixed clipped card", () => {
    const s = defensiveFormationSvg(formation);
    expect(s).toContain('viewBox="-26.67 -18 53.33 26"');
    expect(s).toContain('overflow="hidden"');
    expect(s.match(/class="art-player /g)).toHaveLength(11);
    expect(s.match(/class="art-defender-label"/g)).toHaveLength(11);
    expect(s).toContain('class="art-player art-dl" cx="4.5" cy="-1" r="0.85"');
    expect(s).toContain('class="art-player art-lb" cx="0" cy="-5.5" r="1.15"');
    expect(s).not.toContain('class="art-job"');
    expect(s).not.toMatch(/art-qb|art-route|art-back|reference|target/);
  });

  it("uses the call's own alignment rather than looking up its formation", () => {
    const s = defensivePlaySvg({ ...call, players: [{ slot: "FS", role: "s", x: 3, y: 9 }] });
    expect(s.match(/class="art-player /g)).toHaveLength(1);
    expect(s).toContain('cx="3" cy="-9"');
    expect(s).not.toContain('cx="-6.5"');
  });

  it("draws deep coverage with its authored box, a drop line, and top continuation", () => {
    const s = zoneSvg({ shape: "box", drop: [20, 15], width: 18, depth: [10, 45] });
    expect(s).toContain('class="art-zone art-zone-deep" x="11" y="-45" width="18" height="35"');
    expect(s).toContain('class="art-zone-drop art-deep-line" points="0,-13 20,-15"');
    expect(s).toContain('class="art-zone-drop art-deep-line art-settle"');
    expect(s).toContain('class="art-zone-edge art-deep-line art-head"');
    // Geometry remains intact; the SVG viewport clips both the top and sideline.
    expect(s).toContain('overflow="hidden"');
    expect(s.indexOf('class="art-zone ')).toBeLessThan(s.indexOf('class="art-job"'));
    expect(s.indexOf('class="art-job"')).toBeLessThan(s.indexOf('class="art-defender"'));
  });

  it("does not invent continuation marks for coverage inside or outside the card", () => {
    for (const area of [
      { shape: "box", drop: [0, 10], width: 18, depth: [5, 17] },
      { shape: "box", drop: [0, 30], width: 18, depth: [20, 45] },
      { shape: "box", drop: [50, 15], width: 18, depth: [10, 45] },
      { shape: "capsule", drop: [40, 18], ends: [[40, 18], [40, 18]], radius: 2 },
    ] satisfies ArtZone[]) expect(zoneSvg(area)).not.toContain('class="art-zone-edge');
  });

  it("draws circular and slanted capsules with the authored radius and segment", () => {
    const circle = zoneSvg({ shape: "capsule", drop: [2, 6], ends: [[2, 6], [2, 6]], radius: 3 });
    expect(circle).toContain('class="art-zone art-zone-under" cx="2" cy="-6" r="3"');
    const slant = zoneSvg({ shape: "capsule", drop: [3, 4], ends: [[0, 0], [6, 8]], radius: 2 });
    expect(slant).toContain('width="14" height="4" rx="2" transform="translate(0 0) rotate(-53.13)"');
    const crossing = zoneSvg({ shape: "capsule", drop: [0, 15], ends: [[0, 10], [4, 25]], radius: 2 });
    expect(crossing).toContain('class="art-zone-edge art-under-line art-head"');
  });

  it("cuts a drop beyond the top and ends it with an arrow", () => {
    const s = zoneSvg({ shape: "box", drop: [0, 25], width: 18, depth: [10, 45] });
    expect(s).toContain('class="art-zone-drop art-deep-line" points="0,-13 0,-17.4"');
    expect(s).toContain('class="art-zone-drop art-deep-line art-head"');
    expect(s).not.toContain('art-settle');
    expect(s).not.toContain('art-zone-edge'); // the clipped drop already indicates continuation
  });

  it("distinguishes rush, contain and blitz, and draws stunts as plain rushes", () => {
    const points: [number, number][] = [[2, 1], [2.25, 0], [2.25, -3]];
    const s = defensivePlaySvg({ ...call, jobs: [
      { slot: "DT", kind: "rush", points, contain: false },
      { slot: "SDE", kind: "rush", points, contain: true },
      { slot: "SAM", kind: "blitz", points, contain: false },
      { slot: "NT", kind: "stunt", points, contain: false },
    ] });
    expect(s.match(/class="art-rush" points=/g)).toHaveLength(2);
    expect(s).toContain('class="art-rush art-contain" points="2,-1 2.25,0 2.25,3"');
    expect(s).toContain('class="art-blitz art-head"');
    expect(s).toContain('<title>SDE: Rush, contain</title>');
    expect(s).toContain('<title>SAM: Blitz</title>');
    expect(s).not.toContain('<path'); // no invented stunt loop
  });

  it("shows man rings at the defenders and a spy diamond without offensive targets", () => {
    const s = defensivePlaySvg({ ...call, jobs: [
      { slot: "SCB", kind: "man", at: [14.67, 4.5], leverage: "outside" },
      { slot: "WCB", kind: "man", at: [-14.67, 4.5], leverage: "inside" },
      { slot: "MIKE", kind: "spy", at: [0, 5] },
    ] });
    expect(s.match(/class="art-man"/g)).toHaveLength(2);
    expect(s).toContain('class="art-man" cx="14.67" cy="-4.5" r="1.65"');
    expect(s).toContain('<title>WCB: Man coverage, inside leverage</title>');
    expect(s).toContain('class="art-spy" points="0,-7 2,-5 0,-3 -2,-5"');
    expect(s).not.toContain('<polyline');
    expect(s).not.toMatch(/art-qb|art-back|target|receiver/);
  });

  it("escapes names and labels and produces identical output without mutating input", () => {
    const p: DefensivePlayArt = { ...call, name: '<script>"call" & test</script>',
      players: [{ slot: '<FS>&"', role: "s", x: 0, y: 10 }],
      jobs: [{ slot: '<FS>&"', kind: "man", at: [0, 10], leverage: "inside" }] };
    const saved = JSON.stringify(p);
    const first = defensivePlaySvg(p);
    expect(first).toBe(defensivePlaySvg(p));
    expect(JSON.stringify(p)).toBe(saved);
    expect(first).toContain('&lt;script&gt;&quot;call&quot; &amp; test&lt;/script&gt;');
    expect(first).toContain('&lt;FS&gt;&amp;&quot;');
    expect(first).not.toContain('<script>');
    expect(first).not.toMatch(/\bid=|NaN|Infinity/); // safe to repeat many cards on one page
    expect(defensiveFormationSvg(formation)).toContain('aria-label="Base &lt;front&gt; &quot;A&quot;"');
  });
});
