// Highlight commentary: the key moments of a play as broadcast lines with the
// players' names ("Klubnik throws it, he's targeting Moore... he's open!"),
// shown along the bottom of the field like Football Manager's match text.
// Built from the tracking file's events; everything that isn't a key moment
// (blocks, rush moves, coverage, ...) stays in the Events list only.

import { STRIDE, X, type Recording, type TrackingEvent } from "./tracking";

export type Side = "home" | "away";

export interface Line {
  frame: number; // shown from this frame on
  text: string;
  team: Side; // the team with the ball at that moment (sets the bar's colors)
  big?: boolean; // a touchdown, a turnover, a big hit, ...
  final?: boolean; // the play's summary
}

// "T. Leigh" -> "Leigh", "M. Jones Jr." -> "Jones Jr.".
export function lastName(name: string): string {
  return name.replace(/^([A-Z][a-z]?\.\s*)+/, "").trim() || name;
}

// Gains: "gain of 27 yards", "gain of 1 yard", "no gain", "loss of 3 yards".
export function gain(yards: number): string {
  const n = Math.round(yards);
  if (n === 0) return "no gain";
  const unit = Math.abs(n) === 1 ? "yard" : "yards";
  return n > 0 ? `gain of ${n} ${unit}` : `loss of ${-n} ${unit}`;
}

// After "for": "27 yards", "1 yard", "no gain", "a loss of 3 yards".
export function forYards(yards: number): string {
  const g = gain(yards);
  return g.startsWith("gain of ") ? g.slice(8) : g.startsWith("loss") ? `a ${g}` : g;
}

// Where a team's own goal line is (x, yd) in the field frame: home defends the
// left end when home_right (the sim's default).
function ownGoal(rec: Recording, team: Side): number {
  const homeRight = rec.header.details.home_right !== false;
  return (team === "home") === homeRight ? 10 : 110;
}

// A spot on the field from `team`'s side: "his own 26", "midfield", "the CU18 40".
export function spotName(rec: Recording, team: Side, x: number): string {
  const goal = ownGoal(rec, team);
  const d = goal === 10 ? x - 10 : 110 - x; // yd from his own goal line
  if (d >= 100) return "the end zone";
  if (d <= 0) return "his own end zone";
  const n = Math.min(99, Math.max(1, Math.round(d)));
  if (n === 50) return "midfield";
  if (n < 50) return `his own ${n}`;
  const other = rec.header.teams[team === "home" ? "away" : "home"];
  return `the ${other.abbr} ${100 - n}`;
}

const KICK_EVENTS = new Set(["kick", "punt", "field_goal_try"]);
const TACKLES = new Set(["tackle", "drag_down", "broken_tackle", "missed_tackle", "big_hit"]);

const MOVES: Record<string, (c: string, d: string) => string> = {
  juke: (c, d) => `${c} jukes ${d}!`,
  spin: (c, d) => `${c} spins away from ${d}!`,
  stiff_arm: (c, d) => `${c} stiff-arms ${d}!`,
  cut: (c, d) => `${c} cuts past ${d}!`,
};

const READS: Record<string, (c: string) => string> = {
  bang: (c) => `${c} hits it straight up into the line.`,
  bend: (c) => `${c} cuts it back.`,
  bounce: (c) => `${c} bounces it outside!`,
};

// "Moore..." but "Wesco Jr..." (not four dots).
function dots(name: string): string {
  return name.endsWith(".") ? `${name}..` : `${name}...`;
}

function opennessCall(open: number | undefined): string {
  if (open === undefined || !Number.isFinite(open)) return "";
  if (open >= 0.6) return " He's wide open!";
  if (open >= 0.4) return " He's open!";
  if (open < 0.2) return " Into tight coverage!";
  return " He's covered...";
}

export function buildCommentary(rec: Recording): Line[] {
  const players = rec.header.players;
  const index = new Map(players.map((p, i) => [p.label, i]));
  const name = (label: unknown) => {
    const i = index.get(String(label));
    return i === undefined ? String(label) : lastName(players[i]!.name);
  };
  const teamOf = (label: unknown): Side | undefined => {
    const i = index.get(String(label));
    return i === undefined ? undefined : players[i]!.team;
  };
  // A player's x on an event's frame (for "at the 26").
  const xAt = (label: unknown, frame: number): number | undefined => {
    const i = index.get(String(label));
    if (i === undefined || rec.frameCount === 0) return undefined;
    const f = Math.min(Math.max(frame, 0), rec.frameCount - 1);
    return rec.state[(f * players.length + i) * STRIDE + X];
  };
  const other = (t: Side): Side => (t === "home" ? "away" : "home");

  const kickPlay = "kick_play" in rec.header.details || rec.events.some((e) => KICK_EVENTS.has(e.event));
  const kicker = players.find((p) => p.label === "K" || p.label === "P");
  // The team with the ball: the offense (agents come first) or the kicking team.
  let team: Side = kickPlay && kicker ? kicker.team : (players[0]?.team ?? "home");
  const qb = kickPlay ? undefined : players.find((p) => p.label === "QB" && p.team === team)?.label;

  const lines: Line[] = [];
  const say = (ev: TrackingEvent, text: string, extra: Partial<Line> = {}) =>
    lines.push({ frame: ev.frame, text, team, ...extra });
  // " at his own 26" (or " in the end zone"); `to` for where a run ends up.
  const at = (label: unknown, ev: TrackingEvent, word = "at") => {
    const x = xAt(label, ev.frame);
    if (x === undefined) return "";
    const spot = spotName(rec, teamOf(label) ?? team, x);
    return spot.endsWith("end zone") && word === "at" ? ` in ${spot}` : ` ${word} ${spot}`;
  };
  // The extra point is snapped from the 3 (the header doesn't say it's a try).
  const extraPoint = Number(rec.header.details.yard_line) === 97;

  let carrier: string | undefined = kickPlay ? undefined : qb;
  let passer: string | undefined;
  let target: string | undefined;
  let catcher: string | undefined;
  let thrown = false;
  let handedOff = false;
  let scrambled = false;
  let intercepted: string | undefined;
  let fumbler: string | undefined;
  let recoveredBy: string | undefined;
  let dragging: string | undefined; // a tackler being dragged, waiting for help
  let returner: string | undefined;
  let fieldedX: number | undefined;
  let kickKind = "";
  let fgDistance = 0;
  let lastBreakup = -1;

  for (const ev of rec.events) {
    const f = ev.fields;
    switch (ev.event) {
      case "snap":
        if (qb) say(ev, `${name(qb)} takes the snap.`);
        break;
      case "handoff":
        handedOff = true;
        carrier = String(f.to);
        say(ev, `${name(f.from)} hands it off to ${name(f.to)}.`);
        break;
      case "handoff_missed":
        say(ev, "The handoff is botched!", { big: true });
        break;
      case "read":
        if (handedOff && String(f.player) === carrier && READS[String(f.read)]) say(ev, READS[String(f.read)]!(name(f.player)));
        break;
      case "scramble":
        scrambled = true;
        carrier = String(f.player);
        say(ev, `${name(f.player)} is flushed out... he takes off!`);
        break;
      case "throw": {
        thrown = true;
        passer = String(f.from);
        target = String(f.to);
        carrier = undefined;
        const deep = Number(f.air_yards) >= 20;
        const call = opennessCall(f.openness as number | undefined);
        say(ev, deep
          ? `${name(f.from)} lets it fly deep, he's looking for ${dots(name(f.to))}${call}`
          : `${name(f.from)} throws it, he's targeting ${dots(name(f.to))}${call}`);
        break;
      }
      case "throw_away":
        thrown = true;
        passer = String(f.from);
        say(ev, `${name(f.from)} throws it away.`);
        break;
      case "catch":
        catcher = String(f.player);
        carrier = catcher;
        say(ev, `${name(f.player)} catches it${at(f.player, ev)}!`);
        break;
      case "drop":
        say(ev, `${name(f.player)} drops it!`);
        break;
      case "breakup":
        lastBreakup = ev.tick;
        say(ev, `${name(f.player)} breaks it up!`);
        break;
      case "tip":
        if (ev.tick !== lastBreakup) say(ev, `Tipped by ${name(f.player)}... it's still alive!`);
        break;
      case "interception":
        intercepted = String(f.player);
        carrier = intercepted;
        team = teamOf(f.player) ?? other(team);
        say(ev, `INTERCEPTED! ${name(f.player)} picks it off!`, { big: true });
        break;
      case "out_of_bounds_catch":
        say(ev, `${name(f.player)} can't get his feet down in bounds.`);
        break;
      case "evasion":
        if (f.won === true && MOVES[String(f.move)]) say(ev, MOVES[String(f.move)]!(name(f.player), name(f.on)));
        break;
      case "holding":
        say(ev, `Flag on the play! Holding on ${name(f.player)}.`);
        break;
      case "fumble":
        fumbler = String(f.player);
        carrier = undefined;
        say(ev, `FUMBLE! ${name(f.player)} puts it on the ground!`, { big: true });
        break;
      case "recovery": {
        recoveredBy = String(f.player);
        carrier = recoveredBy;
        const t = teamOf(f.player);
        const theirs = t !== undefined && t !== team;
        if (t) team = t;
        say(ev, `${name(f.player)} recovers it${theirs ? ` for ${rec.header.teams[team].abbr}` : ""}!`, { big: theirs });
        break;
      }

      // Kicks
      case "kick":
        kickKind = String(f.kind);
        say(ev, kickKind === "onside" ? `${name(kicker?.label)} tries the onside kick!`
          : kickKind === "squib" ? `${name(kicker?.label)} squibs it.`
          : `${name(kicker?.label)} kicks it deep.`);
        break;
      case "punt":
        say(ev, Number(f.distance) >= 50 ? `${name(kicker?.label)} booms the punt!` : `${name(kicker?.label)} punts it away.`);
        break;
      case "field_goal_try":
        fgDistance = Math.round(Number(f.distance));
        say(ev, extraPoint ? `${name(kicker?.label)} lines up the extra point...` : `${name(kicker?.label)} lines up the ${fgDistance}-yard field goal...`);
        break;
      case "kick_good":
        say(ev, "It's GOOD!", { big: true });
        break;
      case "kick_no_good":
        say(ev, f.short ? "It's short!" : "It's NO GOOD!", { big: true });
        break;
      case "blocked_kick":
        say(ev, `BLOCKED by ${name(f.player)}!`, { big: true });
        break;
      case "fielded":
        returner = String(f.player);
        carrier = returner;
        fieldedX = xAt(f.player, ev.frame);
        team = teamOf(f.player) ?? other(team);
        say(ev, `${name(f.player)} fields it${at(f.player, ev)}.`);
        break;
      case "fair_catch":
        if (f.player === undefined) break; // the whistle's copy of it
        team = teamOf(f.player) ?? team;
        say(ev, `${name(f.player)} calls for the fair catch${at(f.player, ev)}.`, { final: true });
        break;
      case "muff":
        say(ev, `${name(f.player)} muffs it! The ball is loose!`, { big: true });
        break;
      case "touchback":
        if (lines.at(-1)?.text === "Touchback.") break; // the whistle's copy of it
        if (f.player !== undefined) team = teamOf(f.player) ?? team;
        say(ev, "Touchback.", { final: true });
        break;

      default:
        if (TACKLES.has(ev.event)) tackleLine(ev);
        else if (ev.event === "touchdown") {
          say(ev, `TOUCHDOWN ${rec.header.teams[team].abbr}!`, { big: true });
          summary(ev, true);
        } else if (ev.event === "safety") {
          say(ev, "SAFETY!", { big: true });
        } else if (["tackled", "sack", "out_of_bounds", "incomplete", "down", "recovered", "returned", "made", "missed", "downed"].includes(ev.event)) {
          endLine(ev);
        }
    }
  }
  return lines;

  function tackleLine(ev: TrackingEvent): void {
    const f = ev.fields;
    const by = name(f.player);
    const on = name(f.on);
    const sack = !thrown && !handedOff && !scrambled && !kickPlay && String(f.on) === qb;
    switch (ev.event) {
      case "missed_tackle":
        say(ev, f.dive ? `${by} dives at ${on}... and misses!` : `${by} misses ${on}!`);
        return;
      case "broken_tackle":
        say(ev, `${on} breaks the tackle of ${by}!`);
        return;
      case "big_hit": {
        dragging = undefined;
        // It comes with the tackle on the same tick: say it instead.
        const last = lines.at(-1);
        if (last && last.frame === ev.frame && last.text.startsWith(by)) lines.pop();
        say(ev, sack ? `${by} crushes ${on}! SACKED!` : `BIG HIT! ${by} lays out ${on}!`, { big: true });
        return;
      }
      case "drag_down":
        dragging = String(f.player);
        say(ev, sack ? `${by} gets a hand on ${on}, he drags him down!` : `${by} is on him, he drags him down!`);
        return;
      case "tackle":
        if (dragging === String(f.player)) { /* he already dragged him down */ }
        else if (dragging) say(ev, `${by} comes in to finish the tackle.`);
        else if (sack) say(ev, `${by} gets to ${on}... SACKED!`, { big: true });
        else say(ev, `${by} brings ${on} down.`);
        dragging = undefined;
    }
  }

  // Lines for the whistle: how it ended, then the play's summary.
  function endLine(ev: TrackingEvent): void {
    const f = ev.fields;
    switch (ev.event) {
      case "incomplete":
        if (passer) say(ev, `Incomplete${target ? `, intended for ${name(target)}` : ""}.`, { final: true });
        return;
      case "out_of_bounds":
        if (carrier) say(ev, `${name(carrier)} is pushed out of bounds${at(carrier, ev)}.`);
        break;
      case "down":
        if (carrier) say(ev, `${name(carrier)} goes down${at(carrier, ev)}.`);
        break;
      case "made":
        say(ev, extraPoint ? `${name(kicker?.label)}'s extra point is good.` : `${name(kicker?.label)}'s ${fgDistance}-yard field goal is good.`, { final: true });
        return;
      case "missed":
        say(ev, extraPoint ? `${name(kicker?.label)}'s extra point is no good.` : `${name(kicker?.label)}'s ${fgDistance}-yard field goal is no good.`, { final: true });
        return;
      case "downed":
        say(ev, `The kick is downed${carrier ? at(carrier, ev) : ""}.`, { final: true });
        return;
    }
    summary(ev, false, f.yards as number | undefined);
  }

  function summary(ev: TrackingEvent, touchdown: boolean, yards?: number): void {
    const y = Number(yards ?? ev.fields.yards);
    const has = Number.isFinite(y);
    const td = (n: number) => `${Math.round(n)}-yard`;
    if (returner && ev.event !== "touchdown") {
      const endX = xAt(returner, ev.frame);
      const back = endX !== undefined && fieldedX !== undefined ? Math.abs(endX - fieldedX) : undefined;
      say(ev, `${name(returner)} returns it${back !== undefined ? ` ${forYards(back)}` : ""}${at(returner, ev, "to")}.`, { final: true });
    } else if (returner) {
      say(ev, `${name(returner)} takes it all the way back!`, { final: true, big: true });
    } else if (intercepted) {
      say(ev, `${passer ? `${name(passer)}'s pass is ` : ""}intercepted by ${name(intercepted)}${touchdown ? " and returned for a touchdown!" : "."}`, { final: true });
    } else if (fumbler && recoveredBy && teamOf(recoveredBy) !== teamOf(fumbler)) {
      say(ev, `${name(fumbler)} fumbles, recovered by ${name(recoveredBy)}.`, { final: true });
    } else if (ev.event === "sack" && passer === undefined && qb) {
      say(ev, `${name(qb)} is sacked for ${forYards(has ? Math.min(y, 0) : 0)}.`, { final: true });
    } else if (catcher && passer) {
      say(ev, touchdown && has ? `${name(passer)} to ${name(catcher)}, ${td(y)} touchdown!` : `${name(passer)} to ${name(catcher)}, ${gain(has ? y : 0)}.`, { final: true });
    } else if (carrier) {
      const verb = scrambled ? "scrambles" : "runs";
      say(ev, touchdown && has ? `${name(carrier)} ${td(y)} touchdown ${scrambled ? "scramble" : "run"}!`
        : has ? `${name(carrier)} ${verb} for ${forYards(y)}.` : `${name(carrier)} is down.`, { final: true });
    }
  }
}

// The line showing at `frame`: the last one that has started, or -1.
export function lineAt(lines: Line[], frame: number): number {
  let i = -1;
  for (let k = 0; k < lines.length; k++) {
    if (lines[k]!.frame <= frame) i = k;
    else break;
  }
  return i;
}

// The bar's colors from a team's "#RRGGBB": the color itself (the badge), a
// dark shade of it (the band behind the text, always dark enough for white
// text), and the ink for text on the badge (black on a light color).
export function teamColors(hex: string): { base: string; dark: string; ink: string } {
  const m = /^#?([0-9a-f]{6})$/i.exec(hex.trim());
  const n = m ? parseInt(m[1]!, 16) : 0x555555;
  const rgb = [(n >> 16) & 255, (n >> 8) & 255, n & 255];
  const lum = (0.2126 * rgb[0]! + 0.7152 * rgb[1]! + 0.0722 * rgb[2]!) / 255;
  const k = Math.min(0.55, 0.22 / Math.max(lum, 0.05)); // light colors darken more
  const dark = rgb.map((c) => Math.round(c * k));
  const toHex = (v: number[]) => "#" + v.map((c) => c.toString(16).padStart(2, "0")).join("");
  return { base: toHex(rgb), dark: toHex(dark), ink: lum > 0.55 ? "#111111" : "#ffffff" };
}
