// One-line descriptions of tracking events for the side panel. Events the
// viewer doesn't know get a generic "name key=value ..." line, so new sim
// events show up without a viewer change.

import type { TrackingEvent } from "./tracking";

export function describeEvent(ev: TrackingEvent): string {
  const f = ev.fields;
  switch (ev.event) {
    case "split":
      return `${f.player}: ${f.yards} yd in ${Number(f.time).toFixed(2)} s`;
    default: {
      const rest = Object.entries(f).map(([k, v]) => `${k}=${typeof v === "number" ? round(v) : JSON.stringify(v)}`);
      return [ev.event, ...rest].join(" ");
    }
  }
}

function round(v: number): string {
  return Number.isInteger(v) ? String(v) : v.toFixed(2);
}
