// The Builder's link to cfbsim-server: the same WebSocket and messages as the
// viewer's Plays mode and games, but only the ones the Builder uses.

import type { TeamInfo } from "../plays";
import { rememberServer, serverUrl } from "../server";
import type { CellMessage, MatrixInfo, MatrixSettings } from "./results";
import type { SavedRun } from "./saved";
import type { TeamDetail } from "./teams";

export interface MatrixDone {
  cells: number;
  cancelled: boolean;
  file?: string;
}

// A saved run, opened.
export interface LoadedRun {
  file: string;
  settings: MatrixSettings;
  info: MatrixInfo;
  cancelled: boolean;
  cells: CellMessage[];
}

export interface Playbook {
  id: string;
  name: string;
  side: "offense" | "defense" | "special";
  scheme: string;
  plays: number;
  formations: number;
}

export interface SavedTeamInfo {
  id: string;
  team_file: string;
  roster_file: string;
  created: boolean;
}

export type ConnState = "connecting" | "connected" | "down";

export interface Handlers {
  status(text: string, state: ConnState): void;
  hello(teams: TeamInfo[], version: string): void;
  matrixStarted(info: MatrixInfo, total: number, threads: number): void;
  matrixCell(cell: CellMessage, done: number, total: number): void;
  matrixDone(done: MatrixDone): void;
  matrixList(runs: SavedRun[]): void;
  matrixLoaded(run: LoadedRun): void;
  played(file: string | undefined, summary: string): void; // a run_play reply
  teams(teams: TeamInfo[]): void; // the refreshed list after a team was saved or deleted
  playbooks(list: Playbook[]): void;
  team(detail: TeamDetail): void;
  teamSaved(saved: SavedTeamInfo): void;
  teamDeleted(id: string): void;
  error(message: string, problems: string[]): void;
}

export class Connection {
  private ws: WebSocket | null = null;

  constructor(private on: Handlers) {}

  get open(): boolean {
    return this.ws?.readyState === WebSocket.OPEN;
  }

  connect(url = serverUrl()): void {
    rememberServer(url);
    this.ws?.close();
    this.on.status(`Connecting to ${url}…`, "connecting");
    const ws = new WebSocket(url);
    this.ws = ws;
    ws.onopen = () => this.send({ type: "hello" });
    ws.onerror = () => this.on.status(`Can't reach ${url}. Start the server: ./build/release/server/cfbsim-server`, "down");
    ws.onclose = () => {
      if (this.ws === ws) {
        this.ws = null;
        this.on.status("Disconnected", "down");
      }
    };
    ws.onmessage = (e) => this.receive(JSON.parse(e.data as string));
  }

  send(msg: unknown): boolean {
    if (!this.open) return false;
    this.ws!.send(JSON.stringify(msg));
    return true;
  }

  private receive(m: any): void {
    switch (m.type) {
      case "hello":
        this.on.status(`Connected · sim v${m.version}`, "connected");
        this.on.hello(m.teams as TeamInfo[], m.version as string);
        break;
      case "matrix_started":
        this.on.matrixStarted(m.info as MatrixInfo, m.total as number, m.threads as number);
        break;
      case "matrix_cell":
        this.on.matrixCell({ row: m.row, col: m.col, stats: m.stats, samples: m.samples, applies: m.applies }, m.done as number, m.total as number);
        break;
      case "matrix_done":
        this.on.matrixDone({ cells: m.cells, cancelled: m.cancelled, file: m.file });
        break;
      case "matrix_list":
        this.on.matrixList(m.runs as SavedRun[]);
        break;
      case "matrix_loaded":
        this.on.matrixLoaded(m as LoadedRun);
        break;
      case "play":
        this.on.played(m.file as string | undefined, m.summary as string);
        break;
      case "teams":
        this.on.teams(m.teams as TeamInfo[]);
        break;
      case "playbooks":
        this.on.playbooks(m.playbooks as Playbook[]);
        break;
      case "team":
        this.on.team(m as TeamDetail);
        break;
      case "team_saved":
        this.on.teamSaved(m as SavedTeamInfo);
        break;
      case "team_deleted":
        this.on.teamDeleted(m.id as string);
        break;
      case "error":
        this.on.error(m.message as string, (m.problems as string[] | undefined) ?? []);
        break;
      // Other messages (games) aren't the Builder's.
    }
  }
}
