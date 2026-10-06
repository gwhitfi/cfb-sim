import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import { parseJsonText } from "../playbuilder/paste";
import { fromLibrary, newRoute, onField, problems, routeText, toRoute } from "./route";

type Lib = { routes: Record<string, { points: { x: number; y: number; type?: string; speed?: string }[]; end: string }> };
const lib = parseJsonText(readFileSync(new URL("../../../data/library/routes.json", import.meta.url), "utf8")) as Lib;

describe("route text", () => {
  it("writes back every route in routes.json as the same JSON", () => {
    for (const [name, r] of Object.entries(lib.routes)) {
      const text = routeText(fromLibrary(name, r));
      expect(JSON.parse(`{${text.replace(/,\s*$/, "")}}`)).toEqual({ [name]: r });
    }
  });
  it("writes one point per line, with break and decel", () => {
    const r = { name: "hitch", points: [{ x: 0, y: 5, brk: true, decel: true }, { x: -0.5, y: 4, brk: false, decel: false }], end: "stop" as const };
    expect(routeText(r, "")).toBe(['"hitch": {', '    "points": [', '        { "x": 0, "y": 5, "type": "break", "speed": "decel" },', '        { "x": -0.5, "y": 4 }', "    ],", '    "end": "stop"', "},"].join("\n"));
  });
});

describe("checks", () => {
  it("flags a bad name, no points, a clash, and two breaks", () => {
    expect(problems(newRoute())).toEqual([]);
    expect(problems({ ...newRoute(), name: "deep out" }).join()).toMatch(/letters, digits/);
    expect(problems({ ...newRoute(), points: [] }).join()).toMatch(/at least one point/);
    expect(problems({ ...newRoute(), name: "slant" }, ["slant"]).join()).toMatch(/replaces it/);
    expect(problems({ ...newRoute(), points: newRoute().points.map((p) => ({ ...p, brk: true })) }).join()).toMatch(/more than one break/);
  });
});

describe("the field", () => {
  it("turns +x toward the receiver's sideline, and back", () => {
    const r = newRoute(); // break at (0, 6), then 5 yd inside
    expect(onField(r, 18, -1, "right")[2]).toEqual({ x: 13, y: 9 });
    expect(onField(r, -18, -1, "left")[2]).toEqual({ x: -13, y: 9 });
    expect(toRoute(-13.2, 9.1, -18, -1, "left")).toEqual({ x: -5, y: 10 });
    expect(toRoute(-13.2, 9.1, -18, -1, "left", 0.1)).toEqual({ x: -4.8, y: 10.1 });
  });
});
