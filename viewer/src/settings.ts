// The Defaults (Settings → Defaults…): how the viewer starts. The view, view
// position, game speed, and zoom apply when the page loads; New Game starts
// from them and from auto coach and skip animations; the sidebar sections
// start open or folded. Saved in the browser's localStorage.

import { SPEEDS } from "./playback";
import type { Behind, ViewMode } from "./view";

// The sidebar's foldable sections, by element id, in the order they're listed.
export const SECTIONS = [
  ["sec-stats", "Game stats"],
  ["sec-feed", "Play-by-play"],
  ["sec-recording", "Recording"],
  ["sec-overlays", "Overlays"],
  ["sec-player", "Player"],
  ["sec-events", "Events"],
] as const;

export type SectionId = (typeof SECTIONS)[number][0];

export interface Defaults {
  view: ViewMode;
  behind: Behind;
  speed: number; // one of SPEEDS
  zoom: "fixed" | "fit";
  auto: boolean;
  skip: boolean;
  open: Record<SectionId, boolean>;
}

export const FACTORY: Defaults = {
  view: "horizontal",
  behind: "offense",
  speed: 1,
  zoom: "fixed",
  auto: false,
  skip: false,
  open: {
    "sec-stats": true,
    "sec-feed": true,
    "sec-recording": false,
    "sec-overlays": false,
    "sec-player": false,
    "sec-events": false,
  },
};

const KEY = "cfbsim.defaults";

// Saved defaults, with anything missing or wrong taken from FACTORY.
export function parseDefaults(raw: unknown): Defaults {
  const r = (raw && typeof raw === "object" ? raw : {}) as Record<string, any>;
  const open = { ...FACTORY.open };
  for (const [id] of SECTIONS) if (typeof r.open?.[id] === "boolean") open[id] = r.open[id];
  return {
    view: r.view === "vertical" || r.view === "horizontal" ? r.view : FACTORY.view,
    behind: ["offense", "home", "away"].includes(r.behind) ? r.behind : FACTORY.behind,
    speed: (SPEEDS as readonly number[]).includes(r.speed) ? r.speed : FACTORY.speed,
    zoom: r.zoom === "fit" || r.zoom === "fixed" ? r.zoom : FACTORY.zoom,
    auto: typeof r.auto === "boolean" ? r.auto : FACTORY.auto,
    skip: typeof r.skip === "boolean" ? r.skip : FACTORY.skip,
    open,
  };
}

export function loadDefaults(): Defaults {
  try {
    return parseDefaults(JSON.parse(localStorage.getItem(KEY) ?? "null"));
  } catch {
    return parseDefaults(null);
  }
}

export function saveDefaults(d: Defaults): void {
  try {
    localStorage.setItem(KEY, JSON.stringify(d));
  } catch {
    // No storage: they last until the page closes.
  }
}
