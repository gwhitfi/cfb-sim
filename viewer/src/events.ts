// One-line descriptions of tracking events for the side panel. Events the
// viewer doesn't know get a generic "name key=value ..." line, so new sim
// events show up without a viewer change.

import type { TrackingEvent } from "./tracking";

export function describeEvent(ev: TrackingEvent): string {
  const f = ev.fields;
  switch (ev.event) {
    case "split":
      return `${f.player}: ${f.yards} yd in ${Number(f.time).toFixed(2)} s`;
    case "throw":
      return `throw ${f.from} → ${f.to}, ${yd(f.air_yards)} downfield` +
        (f.openness !== undefined ? ` (open ${Number(f.openness).toFixed(2)})` : "");
    case "throw_away":
      return `${f.from} throws it away`;
    case "interception":
      return `INTERCEPTED by ${f.player} at ${yd(f.air_yards)}`;
    case "breakup":
      return `pass broken up by ${f.player}`;
    case "tip":
      return `tipped by ${f.player}, still live`;
    case "option_read":
      return `${f.player} reads ${f.read}${f.correct ? "" : " (wrong)"}`;
    case "contact":
      return `${f.by} gets to ${f.on}`;
    case "catch":
      return `catch by ${f.player} at ${yd(f.air_yards)}`;
    case "drop":
      return `${f.diving ? "diving " : ""}drop by ${f.player}`;
    case "handoff":
      return `handoff ${f.from} → ${f.to}`;
    case "bust":
      return `BUST ${f.player}: ${f.detail}`;
    case "touchdown":
      return `touchdown (${yd(f.yards)})`;
    case "out_of_bounds":
    case "whistle":
    case "tackled":
    case "sack":
      return `${ev.event.replaceAll("_", " ")}, gain of ${yd(f.yards)}`;
    default: {
      const rest = Object.entries(f).map(([k, v]) => `${k}=${typeof v === "number" ? round(v) : text(v)}`);
      return [ev.event, ...rest].join(" ");
    }
  }
}

function yd(v: unknown): string {
  return `${Number(v).toFixed(1)} yd`;
}

// Strings as they are; anything else as JSON.
export function text(v: unknown): string {
  return typeof v === "string" ? v : JSON.stringify(v);
}

function round(v: number): string {
  return Number.isInteger(v) ? String(v) : v.toFixed(2);
}
