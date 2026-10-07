import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { PlaybookArtStore } from "./art";
import { Live, type GameSetup } from "./live";

// The tests exercise Live's real message/start/replay lifecycle. Its unrelated
// panels and DOM are stubbed; no browser layout is needed for data delivery.
vi.mock("./plays", () => ({ PlayRunner: class { serverStatus() {} } }));
vi.mock("./stats", () => ({ StatsPanel: class { clear() {} } }));

class Socket {
  static OPEN = 1;
  readyState = 1;
  onmessage?: (e: { data: string }) => void;
  send = vi.fn();
  close() {}
  constructor(_url: string) { sockets.push(this); }
}
let sockets: Socket[];
const setup: GameSetup = { home: "home_team", away: "away_team", user: ["home"], auto: false, skip: false };

beforeEach(() => {
  sockets = [];
  const node = () => ({ addEventListener: vi.fn(), replaceChildren: vi.fn(), append: vi.fn(), prepend: vi.fn(),
    classList: { remove: vi.fn() }, hidden: false, checked: false, textContent: "" });
  const nodes = new Map<string, ReturnType<typeof node>>();
  vi.stubGlobal("document", {
    querySelectorAll: () => [],
    getElementById: (id: string) => {
      if (!nodes.has(id)) nodes.set(id, node());
      return nodes.get(id);
    },
    createElement: node,
  });
  vi.stubGlobal("WebSocket", Socket);
});
afterEach(() => vi.unstubAllGlobals());

function session() {
  const live = new Live({ show: () => false, playback: () => null, devMode: () => false, serverInfo: () => {} });
  live.connect("ws://test");
  const socket = sockets[0]!;
  const receive = (m: unknown) => socket.onmessage!({ data: JSON.stringify(m) });
  // Inspect the real store without adding a production accessor solely for tests.
  const store = (live as unknown as { art: PlaybookArtStore }).art;
  const populate = () => {
    for (const side of ["home", "away"]) {
      for (const unit of ["offense", "defense"]) {
        receive({ type: "playbook_art", team: side, unit,
          art: { unit, team: "same_team", playbook: unit, formations: [], plays: [] } });
      }
    }
    expect(store.home.offense).toBeDefined();
    expect(store.home.defense).toBeDefined();
    expect(store.away.offense).toBeDefined();
    expect(store.away.defense).toBeDefined();
  };
  return { live, receive, store, populate };
}

describe("Live playbook art lifecycle", () => {
  it("clears both units at new game and again at started before accepting fresh art", () => {
    const s = session();
    s.populate();
    expect(s.live.start(setup)).toBe(true);
    expect(s.store.home).toEqual({});
    expect(s.store.away).toEqual({});
    s.populate(); // an older reply arriving while the new game request is in flight
    s.receive({ type: "started", mode: "live" });
    expect(s.store.home).toEqual({});
    expect(s.store.away).toEqual({});
    s.populate(); // the new server sends art after started
  });

  it("clears both sides/units when a replay starts", () => {
    const s = session();
    s.populate();
    expect(s.live.watch("game.gamelog.jsonl", false)).toBe(true);
    expect(s.store.home).toEqual({});
    expect(s.store.away).toEqual({});
    s.populate();
    s.receive({ type: "started", mode: "replay" });
    expect(s.store.home).toEqual({});
    expect(s.store.away).toEqual({});
  });

  it("keeps usable pictures when another unit fails", () => {
    const s = session();
    s.populate();
    s.receive({ type: "playbook_art", team: "home", unit: "defense", error: "bad layout" });
    expect(s.store.home.defense).toBeUndefined();
    expect(s.store.home.offense).toBeDefined();
    expect(s.store.away.defense).toBeDefined();
    expect(s.store.away.offense).toBeDefined();
  });
});
