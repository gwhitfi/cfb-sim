// Reads JSON pasted into the Import box: a formation file, one play, or a
// whole playbook (to pick a play from). No DOM (paste.test.ts).

export type Obj = Record<string, unknown>;
export type Side = "offense" | "defense";

export type Pasted =
  | { kind: "formation"; side: Side; json: Obj }
  | { kind: "play"; side: Side; json: Obj }
  | { kind: "playbook"; side: Side; id: string; strengthCall: string[]; plays: Obj[] };

const isObj = (v: unknown): v is Obj => typeof v === "object" && v !== null && !Array.isArray(v);

// The data files allow // comment lines (the libraries use them).
export function parseJsonText(text: string): unknown {
  return JSON.parse(text.replace(/^\s*\/\/.*$/gm, ""));
}

export function readPasted(text: string): Pasted {
  let j: unknown;
  try {
    j = parseJsonText(text);
  } catch (e) {
    throw new Error(`not valid JSON: ${(e as Error).message}`);
  }
  if (!isObj(j)) throw new Error("paste one JSON object: a formation, a play, or a playbook");
  const side = j.side;
  if (side !== "offense" && side !== "defense") {
    throw new Error(side === "special" || side === "kicking" || side === "receiving" ? "special teams aren't built here" : "its 'side' must be offense or defense");
  }
  if (Array.isArray(j.plays)) {
    const plays = j.plays.filter(isObj);
    if (plays.length === 0) throw new Error("the playbook has no plays");
    const call = Array.isArray(j.strength_call) ? j.strength_call.filter((c): c is string => typeof c === "string") : [];
    return { kind: "playbook", side, id: typeof j.id === "string" ? j.id : "", strengthCall: call, plays };
  }
  if (isObj(j.slots)) return { kind: "formation", side, json: j };
  if (isObj(j.assignments)) return { kind: "play", side, json: j };
  throw new Error("couldn't tell what this is: a formation has 'slots', a play 'assignments', a playbook 'plays'");
}
