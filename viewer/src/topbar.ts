// The top bar: New Game, Replays (Play or Game), and the Settings menu
// (Defaults…, Game host…). Each opens a <dialog> from index.html; the live
// game itself runs in live.ts.

import { gameLogTitle, groupByFolder, newestFirst, PLAY_FOLDER, playFileTitle, type FileTitle } from "./files";
import { DEFAULT_SERVER, serverUrl, type GameSetup, type Live } from "./live";
import type { TeamKey } from "./game";
import type { TeamInfo } from "./plays";
import { FACTORY, loadDefaults, saveDefaults, SECTIONS, type Defaults } from "./settings";
import type { Behind, ViewMode } from "./view";

export interface TopBarHost {
  live: Live;
  speeds: readonly number[];
  // A new game's camera and speed, set before it kicks off.
  gameView(view: ViewMode, behind: Behind, speed: number): void;
  // The dev server's tracking files (null without the dev server).
  trackingFiles(): Promise<string[] | null>;
  currentFile(): string | undefined;
  openRepoFile(path: string): void;
  openFile(file: File): void;
  // The defaults changed: apply the view and the sidebar's sections.
  defaultsChanged(d: Defaults): void;
}

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;

const NEW_GAME_KEY = "cfbsim.newgame";
const REPLAYS_TAB_KEY = "cfbsim.replays.tab";

function remember(key: string, value: unknown): void {
  try {
    localStorage.setItem(key, JSON.stringify(value));
  } catch {
    // Not remembered.
  }
}

function recall<T>(key: string): T | null {
  try {
    return JSON.parse(localStorage.getItem(key) ?? "null") as T | null;
  } catch {
    return null;
  }
}

function radio(name: string): string {
  return document.querySelector<HTMLInputElement>(`input[name="${name}"]:checked`)?.value ?? "";
}

function setRadio(name: string, value: string): void {
  const r = document.querySelector<HTMLInputElement>(`input[name="${name}"][value="${value}"]`);
  if (r) r.checked = true;
}

// A list row: the title, a dimmer second line, and the path on hover.
function fileRow(t: FileTitle, path: string, current: boolean, onClick: () => void): HTMLButtonElement {
  const b = document.createElement("button");
  b.type = "button";
  b.className = `file-row${current ? " current" : ""}`;
  b.title = path;
  const title = document.createElement("span");
  title.className = "file-title";
  title.textContent = t.title;
  b.append(title);
  if (t.sub) {
    const sub = document.createElement("span");
    sub.className = "file-sub";
    sub.textContent = t.sub;
    b.append(sub);
  }
  b.addEventListener("click", onClick);
  return b;
}

function note(text: string): HTMLElement {
  const p = document.createElement("p");
  p.className = "muted file-note";
  p.textContent = text;
  return p;
}

const folderName = (dir: string) => (dir === PLAY_FOLDER ? "Plays mode" : dir === "." ? "Repo root" : dir);

export class TopBar {
  defaults = loadDefaults();
  private trackingList: string[] | null = [];

  constructor(private host: TopBarHost) {
    // [data-close] buttons and Escape close a dialog; so does a click on the
    // backdrop. Submitting (Enter, or the dialog's main button) runs its action.
    const actions: Record<string, () => void> = {
      "dlg-game": () => this.kickOff(),
      "dlg-host": () => this.host.live.connect($<HTMLInputElement>("game-server").value.trim() || DEFAULT_SERVER),
    };
    for (const d of document.querySelectorAll<HTMLDialogElement>("dialog.dialog")) {
      d.addEventListener("click", (e) => {
        if (e.target === d) d.close();
      });
      for (const b of d.querySelectorAll("[data-close]")) b.addEventListener("click", () => d.close());
      d.querySelector("form")!.addEventListener("submit", (e) => {
        e.preventDefault();
        actions[d.id]?.();
      });
    }
    this.setupMenu();
    this.setupNewGame();
    this.setupReplays();
    this.setupDefaults();
    this.setupHost();
  }

  // The server's hello: refresh whichever dialog is open.
  serverInfo(): void {
    if ($<HTMLDialogElement>("dlg-game").open) this.fillTeams();
    if ($<HTMLDialogElement>("dlg-replays").open) this.renderReplays();
  }

  // ---------------------------------------------------------------------
  // Settings menu
  // ---------------------------------------------------------------------

  private setupMenu(): void {
    const btn = $("settings");
    const menu = $("settings-menu");
    const show = (open: boolean) => {
      menu.hidden = !open;
      btn.setAttribute("aria-expanded", String(open));
      btn.classList.toggle("active", open);
    };
    btn.addEventListener("click", (e) => {
      e.stopPropagation();
      show(menu.hidden === true);
    });
    document.addEventListener("click", (e) => {
      if (!menu.hidden && !menu.contains(e.target as Node)) show(false);
    });
    document.addEventListener("keydown", (e) => {
      if (e.key === "Escape" && !menu.hidden) show(false);
    });
    for (const item of menu.querySelectorAll<HTMLButtonElement>("[data-open]")) {
      item.addEventListener("click", () => {
        show(false);
        const id = item.dataset.open!;
        if (id === "dlg-defaults") this.openDefaults();
        else if (id === "dlg-host") this.openHost();
      });
    }
  }

  // ---------------------------------------------------------------------
  // New Game
  // ---------------------------------------------------------------------

  private setupNewGame(): void {
    $("new-game").addEventListener("click", () => this.openNewGame());
    $<HTMLSelectElement>("ng-speed").append(...this.host.speeds.map((s) => new Option(`${s}×`, String(s))));
    $("ng-swap").addEventListener("click", () => {
      const home = $<HTMLSelectElement>("ng-home");
      const away = $<HTMLSelectElement>("ng-away");
      [home.value, away.value] = [away.value, home.value];
    });
  }

  private openNewGame(): void {
    const d = this.defaults;
    $<HTMLSelectElement>("ng-speed").value = String(d.speed);
    $<HTMLSelectElement>("ng-view").value = d.view;
    $<HTMLSelectElement>("ng-behind").value = d.behind;
    $<HTMLInputElement>("ng-auto").checked = d.auto;
    $<HTMLInputElement>("ng-skip").checked = d.skip;
    const last = recall<{ side?: string }>(NEW_GAME_KEY);
    if (last?.side) setRadio("ng-side", last.side);
    this.fillTeams();
    this.syncStatus("dlg-game");
    this.host.live.ensureConnected();
    $<HTMLDialogElement>("dlg-game").showModal();
  }

  // The team menus, from the server's teams (keeping the last game's picks).
  private fillTeams(): void {
    const teams = this.host.live.teams;
    const last = recall<{ home?: string; away?: string }>(NEW_GAME_KEY);
    for (const [id, i, saved] of [["ng-home", 0, last?.home], ["ng-away", 1, last?.away]] as const) {
      const sel = $<HTMLSelectElement>(id);
      const keep = sel.value || saved;
      sel.replaceChildren(...teams.map((t) => new Option(t.name, t.id)));
      if (keep && teams.some((t) => t.id === keep)) sel.value = keep;
      else sel.selectedIndex = Math.min(i, teams.length - 1);
    }
    $<HTMLButtonElement>("ng-start").disabled = teams.length === 0;
  }

  private kickOff(): void {
    if (this.host.live.active && !confirm("End the game that's on and start a new one?")) return;
    const side = radio("ng-side");
    const user: TeamKey[] = side === "both" ? ["home", "away"] : side === "none" ? [] : [side as TeamKey];
    const seedText = $<HTMLInputElement>("ng-seed").value.trim();
    const seed = seedText ? Number(seedText) : undefined;
    if (seed !== undefined && !(Number.isInteger(seed) && seed >= 0)) {
      this.dialogStatus("dlg-game", "The seed is a whole number (or blank for a random one)");
      return;
    }
    const g: GameSetup = {
      home: $<HTMLSelectElement>("ng-home").value,
      away: $<HTMLSelectElement>("ng-away").value,
      user,
      seed,
      auto: $<HTMLInputElement>("ng-auto").checked,
      skip: $<HTMLInputElement>("ng-skip").checked,
    };
    this.host.gameView(
      $<HTMLSelectElement>("ng-view").value as ViewMode,
      $<HTMLSelectElement>("ng-behind").value as Behind,
      Number($<HTMLSelectElement>("ng-speed").value),
    );
    if (!this.host.live.start(g)) {
      this.dialogStatus("dlg-game", "Not connected to cfbsim-server (Settings → Game host…)");
      return;
    }
    remember(NEW_GAME_KEY, { home: g.home, away: g.away, side });
    $<HTMLDialogElement>("dlg-game").close();
  }

  private dialogStatus(dialog: string, text: string): void {
    const s = $(dialog).querySelector<HTMLElement>("[data-status]");
    if (!s) return;
    s.textContent = text;
    s.classList.add("bad");
  }

  // ---------------------------------------------------------------------
  // Replays: a play (tracking file) or a game (game log)
  // ---------------------------------------------------------------------

  private setupReplays(): void {
    $("replays").addEventListener("click", () => this.openReplays());
    for (const tab of document.querySelectorAll<HTMLButtonElement>("#dlg-replays [data-tab]")) {
      tab.addEventListener("click", () => this.showTab(tab.dataset.tab!));
    }
    $("rp-filter").addEventListener("input", () => this.renderReplays());
    $<HTMLInputElement>("file-input").addEventListener("change", (e) => {
      const input = e.target as HTMLInputElement;
      const file = input.files?.[0];
      input.value = ""; // so the same file can be picked again
      if (!file) return;
      this.host.openFile(file);
      $<HTMLDialogElement>("dlg-replays").close();
    });
  }

  private openReplays(): void {
    this.showTab(recall<string>(REPLAYS_TAB_KEY) === "game" ? "game" : "play");
    this.syncStatus("dlg-replays");
    this.host.live.ensureConnected();
    this.trackingList = [];
    this.renderReplays();
    $<HTMLDialogElement>("dlg-replays").showModal();
    void this.host.trackingFiles().then((files) => {
      this.trackingList = files;
      this.renderReplays();
    });
  }

  private showTab(tab: string): void {
    for (const t of document.querySelectorAll<HTMLElement>("#dlg-replays [data-tab]")) {
      t.setAttribute("aria-selected", String(t.dataset.tab === tab));
    }
    for (const p of document.querySelectorAll<HTMLElement>("#dlg-replays [data-panel]")) p.hidden = p.dataset.panel !== tab;
    remember(REPLAYS_TAB_KEY, tab);
  }

  private renderReplays(): void {
    const teams: TeamInfo[] = this.host.live.teams;
    const close = () => $<HTMLDialogElement>("dlg-replays").close();

    // Plays, grouped by folder (Plays mode's folder first).
    const plays = $("rp-plays");
    const filter = $<HTMLInputElement>("rp-filter").value.trim().toLowerCase();
    const current = this.host.currentFile();
    if (this.trackingList === null) {
      plays.replaceChildren(note("The file list needs the dev server (npm run dev). Open a file from disk instead."));
    } else {
      const groups = groupByFolder(this.trackingList).flatMap(([dir, files]) => {
        const rows = files
          .map((path) => ({ path, t: playFileTitle(path, teams) }))
          .filter(({ path, t }) => !filter || `${t.title} ${t.sub} ${path}`.toLowerCase().includes(filter))
          .map(({ path, t }) =>
            fileRow(t, path, path === current, () => {
              this.host.openRepoFile(path);
              close();
            }),
          );
        if (!rows.length) return [];
        const head = document.createElement("div");
        head.className = "file-group";
        head.textContent = folderName(dir);
        head.title = dir === "." ? "the repo root" : `${dir}/`;
        return [head, ...rows];
      });
      plays.replaceChildren(...(groups.length ? groups : [note(filter ? "No plays match" : "No play replays yet")]));
    }

    // Games, newest first.
    const games = $("rp-games");
    const live = this.host.live;
    if (!live.connected) {
      games.replaceChildren(note("Not connected to cfbsim-server (Settings → Game host…)"));
    } else if (!live.logs.length) {
      games.replaceChildren(note("No game logs yet"));
    } else {
      games.replaceChildren(
        ...newestFirst(live.logs).map((path) =>
          fileRow(gameLogTitle(path, teams), path, false, () => {
            if (live.active && !confirm("End the game that's on and watch this one?")) return;
            if (live.watch(path, this.defaults.skip)) close();
          }),
        ),
      );
    }
  }

  // ---------------------------------------------------------------------
  // Defaults
  // ---------------------------------------------------------------------

  private setupDefaults(): void {
    $<HTMLSelectElement>("df-speed").append(...this.host.speeds.map((s) => new Option(`${s}×`, String(s))));
    const box = $("df-sections");
    for (const [id, name] of SECTIONS) {
      const label = document.createElement("label");
      label.className = "check";
      const input = document.createElement("input");
      input.type = "checkbox";
      input.dataset.section = id;
      label.append(input, ` ${name}`);
      box.append(label);
    }
    $("dlg-defaults").addEventListener("change", () => this.readDefaults());
    $("df-reset").addEventListener("click", () => {
      this.showDefaults(FACTORY);
      this.readDefaults();
    });
  }

  private openDefaults(): void {
    this.showDefaults(this.defaults);
    $<HTMLDialogElement>("dlg-defaults").showModal();
  }

  private showDefaults(d: Defaults): void {
    setRadio("df-view", d.view);
    setRadio("df-behind", d.behind);
    $<HTMLSelectElement>("df-speed").value = String(d.speed);
    setRadio("df-zoom", d.zoom);
    setRadio("df-auto", d.auto ? "on" : "off");
    setRadio("df-skip", d.skip ? "on" : "off");
    for (const input of document.querySelectorAll<HTMLInputElement>("#df-sections input")) {
      input.checked = d.open[input.dataset.section as keyof Defaults["open"]];
    }
  }

  // Saves what the dialog shows and applies it.
  private readDefaults(): void {
    const open = { ...FACTORY.open };
    for (const input of document.querySelectorAll<HTMLInputElement>("#df-sections input")) {
      open[input.dataset.section as keyof Defaults["open"]] = input.checked;
    }
    this.defaults = {
      view: radio("df-view") as ViewMode,
      behind: radio("df-behind") as Behind,
      speed: Number($<HTMLSelectElement>("df-speed").value),
      zoom: radio("df-zoom") === "fit" ? "fit" : "fixed",
      auto: radio("df-auto") === "on",
      skip: radio("df-skip") === "on",
      open,
    };
    saveDefaults(this.defaults);
    this.host.defaultsChanged(this.defaults);
  }

  // ---------------------------------------------------------------------
  // Game host
  // ---------------------------------------------------------------------

  private setupHost(): void {
    const input = $<HTMLInputElement>("game-server");
    $("host-reset").addEventListener("click", () => (input.value = DEFAULT_SERVER));
  }

  private openHost(): void {
    $<HTMLInputElement>("game-server").value = serverUrl();
    this.syncStatus("dlg-host");
    $<HTMLDialogElement>("dlg-host").showModal();
  }

  // A dialog's status line shows the connection's state.
  private syncStatus(dialog: string): void {
    const live = this.host.live;
    for (const s of $(dialog).querySelectorAll<HTMLElement>("[data-status]")) {
      s.textContent = live.statusText || "Not connected";
      s.classList.toggle("bad", live.statusBad);
    }
  }
}
