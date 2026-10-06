// The Builder page (/generate): create teams and run test matchups against
// cfbsim-server. Its own page, separate from the viewer: tabs for Teams,
// Matchup, and Results, and one connection to the server.

import { serverUrl } from "../server";
import { Connection } from "./connection";
import { el, load, save } from "./dom";
import { doneText, progressText, shortFile } from "./job";
import { kickRequest, type KickForm } from "./kickmatchup";
import { KickMatchupView } from "./kick-matchup-view";
import { matrixRequest, type MatchupForm, type RunPlan } from "./matchup";
import { MatchupView } from "./matchup-view";
import { ResultGrid, countOf, type MatrixSettings, type Sample } from "./results";
import { ResultsView } from "./results-view";
import { TeamsView } from "./teams-view";
import "./builder.css";

const TABS = ["matchup", "results", "teams"] as const; // in the order they're used
type Tab = (typeof TABS)[number];
const TAB_KEY = "cfbsim.builder.tab";
const RATE_KEY = "cfbsim.builder.rate"; // plays per second on the last scrimmage run; ".kicks" for kicks
const MODE_KEY = "cfbsim.builder.matchup.mode";

const $ = <T extends HTMLElement = HTMLElement>(id: string) => document.getElementById(id) as T;

const panels: Record<Tab, HTMLElement> = { teams: $("tab-teams"), matchup: $("tab-matchup"), results: $("tab-results") };
const buttons: Record<Tab, HTMLElement> = { teams: $("btn-teams"), matchup: $("btn-matchup"), results: $("btn-results") };

function showTab(tab: Tab): void {
  for (const t of TABS) {
    panels[t].hidden = t !== tab;
    buttons[t].classList.toggle("active", t === tab);
  }
  save(TAB_KEY, tab);
}
for (const t of TABS) buttons[t].addEventListener("click", () => showTab(t));
const saved = load(TAB_KEY);
showTab(TABS.includes(saved as Tab) ? (saved as Tab) : "matchup");

// ---- the run in progress

let running = false;
let started = 0; // performance.now() at matrix_started
let grid: ResultGrid | null = null;
let lastRequest: MatrixSettings | null = null; // what the run in progress was asked for
let watchWindow: Window | null = null; // a tab opened for a Watch, waiting for its play
let compareFile: string | null = null; // a server run asked for to compare with (A), not to open

const jobBar = $("job");
const progress = $<HTMLProgressElement>("job-progress");
const jobText = $("job-text");
const jobFile = $("job-file");
const cancel = $<HTMLButtonElement>("job-cancel");
const connButton = $("conn-button");
cancel.addEventListener("click", () => conn.send({ type: "cancel_matrix" }));

// The job strip's line; `file`: a saved run it names, shown by its short name (the full path in its tooltip).
function jobStatus(text: string, bad = false, file: string | null = null): void {
  jobBar.hidden = false;
  jobText.textContent = text;
  jobText.classList.toggle("bad", bad);
  jobFile.hidden = !file;
  jobFile.textContent = file ? shortFile(file) : "";
  jobFile.title = file ?? "";
}

const teamsView = new TeamsView(panels.teams, {
  select: (id) => conn.send({ type: "get_team", id }),
  save: (request) => {
    if (!conn.send(request)) {
      teamsView.problems("Not connected to the server", []);
    }
  },
  remove: (id) => {
    if (!conn.send({ type: "delete_team", id })) teamsView.problems("Not connected to the server", []);
  },
});
const results = new ResultsView(panels.results, {
  // Replays one play of a cell (its seed, hash, and flip) on the server, then opens it in the viewer.
  watch(grid: ResultGrid, sample: Sample, row: number, col: number) {
    const s = grid.settings;
    const play = grid.info.rows[row]!.id;
    const answer = grid.info.cols[col]!.id;
    const sent = conn.send(
      grid.domain === "kicks"
        ? {
            type: "run_kick",
            kicking: s.kicking,
            kick: play,
            receiving: s.receiving,
            return: answer,
            seed: sample.seed,
            hash: sample.hash,
            ...(s.yard_line !== undefined ? { yard_line: s.yard_line } : {}),
            return_aggression: s.return_aggression ?? 50,
          }
        : {
            type: "run_play",
            offense: s.offense,
            play,
            ...(s.defense && answer ? { defense: s.defense, def_play: answer } : {}),
            seed: sample.seed,
            hash: sample.hash,
            flip: sample.flip,
            yard_line: s.yard_line ?? 25,
            huddle: s.huddle ?? false,
          },
    );
    if (!sent) return jobStatus("Not connected to the server", true);
    // Open the tab now, in the click, so the browser allows it; it gets the play when the server answers.
    watchWindow?.close();
    watchWindow = window.open("about:blank", "_blank");
    jobStatus(`Running the ${sample.kind} play…`);
  },
  openSaved(file: string) {
    compareFile = null;
    conn.send({ type: "get_matrix", file });
    jobStatus("Opening", false, file);
  },
  compareSaved(file: string) {
    compareFile = file;
    if (!conn.send({ type: "get_matrix", file })) return jobStatus("Not connected to the server", true);
    jobStatus("Opening to compare:", false, file);
  },
});

// The Matchup tab has two halves, plays and kicks, behind a switch.
const scrimmageRoot = el("div");
const kickRoot = el("div");
const modeButtons = {
  scrimmage: el("button", { textContent: "Plays", title: "Offensive plays against defensive plays" }),
  kicks: el("button", { textContent: "Kicks", title: "Kickoffs, punts, and field goals against returns and blocks" }),
};
function showMode(mode: "scrimmage" | "kicks"): void {
  scrimmageRoot.hidden = mode !== "scrimmage";
  kickRoot.hidden = mode !== "kicks";
  modeButtons.scrimmage.classList.toggle("active", mode === "scrimmage");
  modeButtons.kicks.classList.toggle("active", mode === "kicks");
  save(MODE_KEY, mode);
}
modeButtons.scrimmage.addEventListener("click", () => showMode("scrimmage"));
modeButtons.kicks.addEventListener("click", () => showMode("kicks"));
panels.matchup.append(el("div", { className: "modebar" }, modeButtons.scrimmage, modeButtons.kicks), scrimmageRoot, kickRoot);
showMode(load(MODE_KEY) === "kicks" ? "kicks" : "scrimmage");

// Sends a run_matrix request and marks the run started; `settings` is what the run was asked for.
function start(request: Record<string, unknown>): void {
  if (running) return;
  const { type: _type, ...settings } = request;
  lastRequest = settings as MatrixSettings;
  if (!conn.send(request)) {
    jobStatus("Not connected to the server", true);
    return;
  }
  running = true;
  matchup.setRunning(true);
  kickMatchup.setRunning(true);
  jobStatus("Starting…");
}

const matchup = new MatchupView(scrimmageRoot, {
  run: (form: MatchupForm, plan: RunPlan) => start(matrixRequest(form, plan)),
});
const kickMatchup = new KickMatchupView(kickRoot, {
  run: (form: KickForm, plan: RunPlan) => start(kickRequest(form, plan)),
});
const rateOf = (key: string) => {
  const r = load(key);
  return typeof r === "number" ? r : null;
};
matchup.setRate(rateOf(RATE_KEY));
kickMatchup.setRate(rateOf(`${RATE_KEY}.kicks`));

const conn: Connection = new Connection({
  status(text, state) {
    $("conn-status").textContent = text;
    $("conn-status").classList.toggle("bad", state === "down");
    connButton.className = `conn-button ${state}`;
    connButton.title = `${text} · click to change the server's address`;
    $("conn-label").textContent = state === "connected" ? "Connected" : state === "connecting" ? "Connecting…" : "Not connected";
  },
  hello(teams) {
    teamsView.setTeams(teams);
    matchup.setTeams(teams);
    kickMatchup.setTeams(teams);
    results.setTeams(teams);
    conn.send({ type: "list_matrix" });
    conn.send({ type: "list_playbooks" });
  },
  teams(teams) {
    // A team was saved or deleted: every list that shows teams needs the new one.
    teamsView.setTeams(teams);
    matchup.setTeams(teams);
    kickMatchup.setTeams(teams);
    results.setTeams(teams);
  },
  playbooks(list) {
    teamsView.setPlaybooks(list);
  },
  team(detail) {
    teamsView.showTeam(detail);
  },
  teamSaved(saved) {
    teamsView.saved(saved.id, saved.created);
  },
  teamDeleted(id) {
    teamsView.deleted(id);
  },
  matrixStarted(info, total, threads) {
    grid = new ResultGrid(info, lastRequest ?? { offense: info.offense, defense: info.defense });
    started = performance.now();
    results.setGrid(grid, "live run");
    progress.max = total;
    progress.value = 0;
    cancel.hidden = false;
    jobStatus(`${progressText(0, total, 0)} · ${threads} thread${threads === 1 ? "" : "s"}`);
    showTab("results");
  },
  matrixCell(cell, done, total) {
    grid?.add(cell);
    progress.value = done;
    jobStatus(progressText(done, total, (performance.now() - started) / 1000));
    results.schedule();
  },
  matrixDone(done) {
    running = false;
    matchup.setRunning(false);
    kickMatchup.setRunning(false);
    cancel.hidden = true;
    const seconds = (performance.now() - started) / 1000;
    // Plays or kicks actually run (a cell that doesn't apply has none): the speed for the time estimate.
    let plays = 0;
    for (const row of grid?.cells ?? []) for (const cell of row) plays += cell ? countOf(cell.stats) : 0;
    if (!done.cancelled && seconds > 0.2 && plays > 0) {
      const rate = plays / seconds;
      if (grid?.domain === "kicks") {
        save(`${RATE_KEY}.kicks`, rate);
        kickMatchup.setRate(rate);
      } else {
        save(RATE_KEY, rate);
        matchup.setRate(rate);
      }
    }
    const end = doneText(done, seconds);
    jobStatus(end.file ? `${end.text} · saved` : end.text, false, end.file);
    results.schedule();
    conn.send({ type: "list_matrix" });
  },
  matrixList(runs) {
    results.setSaved(runs);
  },
  matrixLoaded(run) {
    const short = run.file.split("/").pop() ?? run.file;
    if (run.file === compareFile) {
      compareFile = null;
      const a = new ResultGrid(run.info, run.settings);
      for (const cell of run.cells) a.add(cell);
      const why = results.compareWith(a, `saved run ${short}`);
      if (why) jobStatus(why, true);
      else jobStatus("Comparing with", false, run.file);
      showTab("results");
      return;
    }
    grid = new ResultGrid(run.info, run.settings);
    for (const cell of run.cells) grid.add(cell);
    results.setGrid(grid, `saved run ${short}${run.cancelled ? " (cancelled)" : ""}`);
    jobStatus("Opened", false, run.file);
    showTab("results");
  },
  played(file, summary) {
    const w = watchWindow;
    watchWindow = null;
    if (!w) return; // not one of ours
    if (!file) {
      w.close();
      return jobStatus("The server didn't save the play to watch", true);
    }
    w.location.href = `/#file=${encodeURIComponent(file)}`;
    jobStatus(summary);
  },
  error(message, problems) {
    if (teamsView.problems(message, problems)) return; // a refused team save or delete
    const wasWatch = watchWindow !== null; // a Watch that failed doesn't stop a matrix run
    watchWindow?.close();
    watchWindow = null;
    if (!wasWatch) {
      running = false;
      matchup.setRunning(false);
      kickMatchup.setRunning(false);
      cancel.hidden = true;
    }
    jobStatus(message, true);
  },
});

// The connection dot: a click shows the address and Connect.
const address = $<HTMLInputElement>("conn-url");
const connPop = $("conn-pop");
address.value = serverUrl();
const connect = () => {
  connPop.hidden = true;
  conn.connect(address.value.trim() || undefined);
};
connButton.addEventListener("click", () => {
  connPop.hidden = !connPop.hidden;
  if (!connPop.hidden) address.select();
});
$("conn-go").addEventListener("click", connect);
address.addEventListener("keydown", (e) => {
  if (e.key === "Enter") connect();
  if (e.key === "Escape") connPop.hidden = true;
});
document.addEventListener("mousedown", (e) => {
  if (!connPop.hidden && !(e.target as HTMLElement).closest(".conn")) connPop.hidden = true;
});
conn.connect();
