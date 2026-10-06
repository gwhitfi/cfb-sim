// A route template as data/library/routes.json writes it: waypoints in the
// route frame (origin at the receiver, +x toward his sideline, +y downfield)
// and how it ends. No DOM (route.test.ts).

export type End = "continue" | "stop" | "settle";
export const ENDS: readonly End[] = ["continue", "stop", "settle"];
export const END_HELP: Record<End, string> = {
  continue: "keeps going on the last leg until the throw or the sideline",
  stop: "stops at the last point and faces the QB",
  settle: "finds open grass near the last point",
};

export interface Waypoint {
  x: number;
  y: number;
  brk: boolean; // a planted cut: where Route Running and Agility make separation
  decel: boolean; // throttles down into this point (a curl, a hitch)
}
export interface RouteDraft {
  name: string;
  points: Waypoint[];
  end: End;
}

export const newRoute = (): RouteDraft => ({ name: "new_route", points: [{ x: 0, y: 6, brk: true, decel: false }, { x: -5, y: 10, brk: false, decel: false }], end: "continue" });

// A template from the library ({"points": [...], "end": ...}).
export function fromLibrary(name: string, j: { points: { x: number; y: number; type?: string; speed?: string }[]; end: string }): RouteDraft {
  return {
    name,
    points: j.points.map((p) => ({ x: p.x, y: p.y, brk: p.type === "break", decel: p.speed === "decel" })),
    end: (ENDS as readonly string[]).includes(j.end) ? (j.end as End) : "continue",
  };
}

// What the loader would reject, plus names that would clash.
export function problems(r: RouteDraft, existing: readonly string[] = []): string[] {
  const out: string[] = [];
  if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(r.name)) out.push("the name should be letters, digits, and _ (like deep_out)");
  else if (existing.includes(r.name)) out.push(`routes.json already has '${r.name}': pasting this replaces it`);
  if (r.points.length === 0) out.push("a route needs at least one point");
  if (r.points.filter((p) => p.brk).length > 1) out.push("more than one break: a depth override only moves the route from the first one");
  return out;
}

const n = (v: number) => String(Math.round(v * 100) / 100 + 0);

// The entry to paste into routes.json's "routes", one point per line, with
// the file's 4-space indent and a trailing comma.
export function routeText(r: RouteDraft, indent = "        "): string {
  const pt = (p: Waypoint) => `{ "x": ${n(p.x)}, "y": ${n(p.y)}${p.brk ? ', "type": "break"' : ""}${p.decel ? ', "speed": "decel"' : ""} }`;
  const i2 = indent + "    ";
  const i3 = i2 + "    ";
  return [
    `${indent}"${r.name}": {`,
    `${i2}"points": [`,
    r.points.map((p) => i3 + pt(p)).join(",\n"),
    `${i2}],`,
    `${i2}"end": "${r.end}"`,
    `${indent}},`,
  ].join("\n");
}

// The route on the field from a receiver at (x, y): +x of the template turns
// toward his sideline (`side`).
export function onField(r: RouteDraft, x: number, y: number, side: "left" | "right"): { x: number; y: number }[] {
  const s = side === "left" ? -1 : 1;
  return [{ x, y }, ...r.points.map((p) => ({ x: x + s * p.x, y: y + p.y }))];
}

// The other way: a field spot to route-frame coordinates, snapped.
export function toRoute(fx: number, fy: number, x: number, y: number, side: "left" | "right", step = 0.5): { x: number; y: number } {
  const snap = (v: number) => Math.round(Math.round(v / step) * step * 100) / 100 + 0;
  return { x: snap((fx - x) * (side === "left" ? -1 : 1)), y: snap(fy - y) };
}
