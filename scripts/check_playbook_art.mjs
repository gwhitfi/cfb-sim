// Integration check against the real WebSocket server and viewer decoder.
// Run from the repository root (Node >= 22):
// node --experimental-strip-types scripts/check_playbook_art.mjs [server-executable]
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { once } from "node:events";
import { cp, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { createServer } from "node:net";
import { tmpdir } from "node:os";
import { resolve, join } from "node:path";
import { fileURLToPath } from "node:url";
import { readArtMessage, PlaybookArtStore } from "../viewer/src/art.ts";

const root = fileURLToPath(new URL("../", import.meta.url));
const executable = resolve(process.argv[2] ?? join(root, "build/debug/server/cfbsim-server"));
const scratch = await mkdtemp(join(tmpdir(), "cfbsim-art-"));
const data = join(scratch, "data");
let server;
let stopped;
let diagnostics = "";

function deadline(promise, label) {
  let timer;
  return Promise.race([promise, new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error(`${label} timed out\n${diagnostics}`)), 30000);
  })]).finally(() => clearTimeout(timer));
}

async function messages(user) {
  const socket = new WebSocket(url);
  const replies = [];
  try {
    await deadline(new Promise((done, reject) => {
      socket.addEventListener("error", () => reject(new Error("WebSocket error")));
      socket.addEventListener("open", () => socket.send(JSON.stringify({
        type: "new_game", home: "clemson_2018", away: "clemson_2025", seed: 1, user,
      })));
      socket.addEventListener("message", (event) => {
        const m = JSON.parse(event.data);
        replies.push(m);
        if (m.type === "error") return reject(new Error(m.message));
        if (m.type === "started") {
          // Like Live.begin: request progress as soon as started arrives.
          if (user.length) {
            socket.send(JSON.stringify({ type: "command", command: { type: "run_to", tick: 0 } }));
            socket.send(JSON.stringify({ type: "advance" }));
          }
          // A barrier: all earlier replies must arrive before this hello.
          socket.send(JSON.stringify({ type: "hello" }));
        }
        if (m.type === "hello") done();
      });
    }), "new game replies");
    return replies;
  } finally {
    socket.close();
  }
}

function verify(replies, user, brokenDefense = false) {
  assert.equal(replies[0].type, "started");
  const expected = user.flatMap((side) => ["offense", "defense"].map((unit) => [side, unit]));
  const art = replies.filter((r) => r.type === "playbook_art");
  assert.deepEqual(art.map((r) => [r.team, r.unit]), expected);
  assert.deepEqual(replies.slice(1, 1 + expected.length), art, "all pictures follow started before progress");
  const store = new PlaybookArtStore();
  for (const reply of art) {
    const decoded = readArtMessage(reply);
    assert.ok(decoded, "the viewer understands the server's art envelope");
    if (brokenDefense && reply.team === "home" && reply.unit === "defense") {
      assert.equal(decoded.art, null);
      assert.match(decoded.error, /anchored to itself/);
      assert.equal(reply.art, undefined);
    } else {
      assert.ok(decoded.art);
      assert.equal(decoded.error, "");
      assert.equal(decoded.art.team, reply.team === "home" ? "clemson_2018" : "clemson_2025");
    }
    store.receive(reply);
  }
  if (user.length) {
    const progress = replies.find((r) => r.type === "advance");
    assert.ok(progress, "the match still advances");
    assert.ok(replies.some((r) => r.type === "command" && r.ok));
    assert.ok(!JSON.stringify(progress.result).includes('"playbook_art"'), "art stays outside match results");
  }
  if (brokenDefense) {
    assert.ok(store.home.offense);
    assert.equal(store.home.defense, undefined);
    assert.ok(store.away.offense);
    assert.ok(store.away.defense);
  }
}

let url;
try {
  await cp(join(root, "data"), data, { recursive: true });
  const reservation = createServer();
  reservation.listen(0, "127.0.0.1");
  await once(reservation, "listening");
  const port = reservation.address().port;
  await new Promise((done, reject) => reservation.close((e) => e ? reject(e) : done()));
  url = `ws://127.0.0.1:${port}`;
  server = spawn(executable, ["--host", "127.0.0.1", "--port", String(port), "--data", data,
    "--out", join(scratch, "games")], { cwd: root, stdio: ["ignore", "pipe", "pipe"] });
  stopped = once(server, "exit");
  await deadline(new Promise((done, reject) => {
    server.on("error", reject);
    server.on("exit", (code) => reject(new Error(`server exited (${code})\n${diagnostics}`)));
    server.stderr.on("data", (chunk) => { diagnostics += chunk; });
    server.stdout.on("data", (chunk) => {
      diagnostics += chunk;
      if (diagnostics.includes("listening on ws://")) done();
    });
  }), "server startup");

  for (const user of [["home"], ["away"], ["home", "away"], []]) verify(await messages(user), user);
  console.log("PASS: art for each coached side/unit, no art for spectators, and replies before advance");

  // Isolate an art-layout failure to home defense. This call override passes
  // data reference checks, but cannot be laid out because SS depends on itself.
  // Only the temporary data copy is modified, and no snap is requested.
  const book = JSON.parse(await readFile(join(data, "playbooks/43_base.json"), "utf8"));
  book.id = "broken_art";
  book.plays[0].assignments.SS.align = { x: { from: "SS", offset: 0 } };
  await writeFile(join(data, "playbooks/broken_art.json"), JSON.stringify(book));
  const teamPath = join(data, "teams/clemson_2018.json");
  const team = JSON.parse(await readFile(teamPath, "utf8"));
  team.playbooks.defense = book.id;
  await writeFile(teamPath, JSON.stringify(team));
  verify(await messages(["home", "away"]), ["home", "away"], true);
  console.log("PASS: defensive art error preserves the game and the other three art entries");
} finally {
  if (server && server.exitCode === null) {
    server.kill("SIGTERM");
    await deadline(stopped, "server shutdown");
  }
  await rm(scratch, { recursive: true, force: true });
}
