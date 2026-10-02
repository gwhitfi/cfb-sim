// Live games and full-game playback (phase 11): the server's messages, the
// game state, play-by-play text, and the queue that plays a game's snaps
// back one at a time. No DOM here (see live.ts), so it can be unit tested.

export type TeamKey = "home" | "away";

export interface GameState {
  tick: number;
  quarter: number;
  overtime: boolean;
  clock: number;
  clock_text: string;
  clock_running: boolean;
  play_clock: number;
  teams: Record<TeamKey, { id: string; name: string; abbr: string }>;
  score: Record<TeamKey, number>;
  timeouts: Record<TeamKey, number>;
  offense: TeamKey;
  down: number;
  distance: number;
  goal_to_go: boolean;
  yard_line: number;
  situation: string;
  hash: string;
  offense_right: boolean;
}

export interface PlayOption {
  id: string;
  name: string;
  formation: string;
  formation_name?: string;
  type: string;
  flip: boolean;
  tags: string[];
  personnel?: string;
  package?: string;
}

export interface Prompt {
  input: "call_play" | "hike" | "coaching_decision" | "penalty_decision" | "runoff" | "timeout";
  team: TeamKey;
  suggestion: any;
  options: any;
  state: GameState;
}

export interface AdvanceResult {
  messages: ({ type: "line"; line: any; state: GameState } | { type: "tracking"; name: string; data: string })[];
  prompt: Prompt | null;
  done: boolean;
  error?: string;
}

// One thing to show, in order: a snap to animate, or a log line to apply.
export type Step =
  | { kind: "tracking"; name: string; data: string }
  | { kind: "line"; line: any; state: GameState };

// The game's snaps and log lines waiting to be shown. Lines between snaps
// are applied at once; a snap waits for its animation to finish.
export class StepQueue {
  private steps: Step[] = [];

  push(result: AdvanceResult): void {
    for (const m of result.messages) {
      if (m.type === "tracking") this.steps.push({ kind: "tracking", name: m.name, data: m.data });
      else this.steps.push({ kind: "line", line: m.line, state: m.state });
    }
  }

  get empty(): boolean {
    return this.steps.length === 0;
  }

  get length(): number {
    return this.steps.length;
  }

  shift(): Step | undefined {
    return this.steps.shift();
  }

  clear(): void {
    this.steps = [];
  }
}

const ORDINAL = ["1st", "2nd", "3rd", "4th"];

export function periodText(s: GameState): string {
  if (s.overtime) return `OT${s.quarter - 4 > 1 ? s.quarter - 4 : ""}`;
  return `${ORDINAL[Math.min(Math.max(s.quarter, 1), 4) - 1]}`;
}

export function downText(s: GameState): string {
  const d = ORDINAL[Math.min(Math.max(s.down, 1), 4) - 1];
  return `${d} & ${s.goal_to_go ? "Goal" : Math.max(1, Math.round(s.distance))}`;
}

const PENALTY_NAMES: Record<string, string> = {
  false_start: "False start",
  offside: "Offside",
  delay_of_game: "Delay of game",
  offensive_holding: "Holding (offense)",
  defensive_holding: "Holding (defense)",
  pass_interference: "Pass interference",
  defensive_pass_interference: "Pass interference (defense)",
  offensive_pass_interference: "Pass interference (offense)",
  roughing_the_passer: "Roughing the passer",
  facemask: "Face mask",
};

export function penaltyName(type: string): string {
  return PENALTY_NAMES[type] ?? type.replace(/_/g, " ").replace(/^./, (c) => c.toUpperCase());
}

// One play-by-play line for a game log line (null: nothing to show).
// `names`: player id -> name, from the tracking files seen so far.
export function describeLine(line: any, state: GameState, names: Map<string, string> = new Map()): string | null {
  const abbr = (k: unknown) => (k === "home" || k === "away" ? state.teams[k].abbr : String(k ?? ""));
  const player = (id: unknown) => names.get(String(id)) ?? String(id ?? "");
  switch (line.type) {
    case "header":
      return `${state.teams.away.abbr} at ${state.teams.home.abbr} (seed ${line.seed})`;
    case "input":
      return null;
    case "play": {
      if (line.try) return `${abbr(line.offense)} two-point try: ${line.summary}`;
      const who = line.kicking ? abbr(line.kicking) : abbr(line.offense);
      const extra = [
        line.audible_from ? `audible from ${line.audible_from}` : "",
        line.hot_routes ? `hot routes ${Object.entries(line.hot_routes).map(([s, r]) => `${s} ${r}`).join(", ")}` : "",
      ].filter(Boolean);
      const fouls = (line.fouls ?? [])
        .filter((f: any) => f.flagged)
        .map((f: any) => `FLAG: ${penaltyName(f.foul)} ${abbr(f.team)}${f.accepted ? "" : " (declined)"}`);
      return [`${who} ${line.situation}: ${line.summary}`, ...extra.map((e) => `(${e})`), ...fouls].join(" ");
    }
    case "event":
      switch (line.event) {
        case "coin_toss":
          return `${abbr(line.winner)} wins the toss; ${abbr(line.receives)} receives`;
        case "timeout":
          return `Timeout ${abbr(line.team)} (${line.left} left) at ${line.clock}`;
        case "two_minute_timeout":
          return "Two-minute timeout";
        case "penalty":
          return `Penalty: ${penaltyName(line.foul)}, ${abbr(line.team)}, ${Math.abs(line.yards)} yd. ${line.result}${line.runoff ? ` (${line.runoff}-second runoff)` : ""}`;
        case "offsetting_fouls":
          return "Offsetting fouls: replay the down";
        case "runoff":
          return `${line.seconds}-second runoff`;
        case "extra_point":
          return `Extra point ${line.good ? "good" : "no good"} (${abbr(line.team)})`;
        case "two_point_try":
          return `Two-point try ${line.good ? "good" : "failed"} (${abbr(line.team)})`;
        case "end_of_quarter":
          return `End of quarter ${line.quarter}: ${state.teams.home.abbr} ${line.home}, ${state.teams.away.abbr} ${line.away}`;
        case "halftime":
          return "Halftime";
        case "clock_expired":
          return "Time runs out";
        case "end_of_regulation":
          return "End of regulation";
        case "overtime":
          return `Overtime ${line.period}`;
        case "overtime_possession":
          return `${abbr(line.offense)} ball at ${line.spot}`;
        case "injury":
          return `Injury: ${player(line.player)} (${abbr(line.team)})`;
        case "sub_out":
          return `${abbr(line.team)} sub: ${player(line.player)} out (${Math.round(line.energy * 100)}% energy)`;
        case "sub_in":
          return `${abbr(line.team)} sub: ${player(line.player)} back in`;
        case "final":
          return `Final: ${state.teams.home.abbr} ${line.home}, ${state.teams.away.abbr} ${line.away}`;
        default:
          return line.event;
      }
    default:
      return null;
  }
}

// Does this log line end a scoring play?
export function isScore(line: any): boolean {
  return line.type === "play" && line.home !== undefined;
}

// Plays grouped by formation, in playbook order.
export function byFormation(plays: PlayOption[]): [string, PlayOption[]][] {
  const groups = new Map<string, PlayOption[]>();
  for (const p of plays) {
    const key = p.formation_name ?? p.formation;
    if (!groups.has(key)) groups.set(key, []);
    groups.get(key)!.push(p);
  }
  return [...groups.entries()];
}

// The prompt's input as a short heading.
export function promptTitle(p: Prompt): string {
  switch (p.input) {
    case "call_play":
      return p.options.side === "offense" ? (p.options.try ? "Two-point play" : "Call the play") : "Call the defense";
    case "hike":
      return "At the line";
    case "coaching_decision":
      return (
        { fourth_down: "Fourth down", try: "After the touchdown", kickoff: "Kickoff", coin_toss: "You won the toss" } as Record<string, string>
      )[p.options.decision] ?? p.options.decision;
    case "penalty_decision":
      return "Flag on the play";
    case "runoff":
      return "10-second runoff?";
    default:
      return p.input;
  }
}

export const CHOICE_NAMES: Record<string, string> = {
  go: "Go for it",
  punt: "Punt",
  field_goal: "Field goal",
  kick: "Kick the PAT",
  two: "Go for two",
  deep: "Deep kick",
  squib: "Squib",
  onside: "Onside",
  receive: "Receive",
  defer: "Defer",
};

// The play clock reading when the offense is set, by tempo.
export function readyReading(options: any, tempo: string): number {
  const r = options.ready ?? {};
  return Number(r[tempo] ?? r.huddle ?? options.play_clock ?? 40);
}
