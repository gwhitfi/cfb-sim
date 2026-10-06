import { readdir, readFile } from "node:fs/promises";
import { join, relative, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { defineConfig, type Plugin } from "vite";

// The repo root, where `cfbsim run-drill ... --out x.jsonl` writes by default.
const REPO = fileURLToPath(new URL("..", import.meta.url));
// Folders searched for tracking files: the repo root, out/, and two levels of
// folders inside out/ (a game's per-snap files from `--tracking out/NAME`, or
// grouped test runs like out/tests/smash).
const SEARCH = [".", "out"];

// Dev-server only: lists and serves the repo's tracking files, so the viewer
// can open them from a menu and reload them after rerunning the CLI. Only
// *.jsonl files inside the search folders are served.
function trackingFiles(): Plugin {
  return {
    name: "cfbsim-tracking-files",
    configureServer(server) {
      server.middlewares.use("/api/tracking", async (req, res) => {
        try {
          const url = new URL(req.url ?? "/", "http://localhost");
          if (url.pathname === "/" || url.pathname === "") {
            res.setHeader("Content-Type", "application/json");
            res.end(JSON.stringify(await listFiles()));
            return;
          }
          if (url.pathname === "/file") {
            const path = resolve(REPO, url.searchParams.get("path") ?? "");
            const allowed = await listFiles();
            const rel = relative(REPO, path).split(sep).join("/");
            if (!allowed.includes(rel)) {
              res.statusCode = 404;
              res.end("not a tracking file in the repo");
              return;
            }
            res.setHeader("Content-Type", "text/plain; charset=utf-8");
            res.setHeader("Cache-Control", "no-store");
            res.end(await readFile(path));
            return;
          }
          res.statusCode = 404;
          res.end("not found");
        } catch (e) {
          res.statusCode = 500;
          res.end(String(e));
        }
      });
    },
  };
}

async function listFiles(): Promise<string[]> {
  const found: string[] = [];
  const scan = async (dir: string, depth: number) => {
    let entries;
    try {
      entries = await readdir(join(REPO, dir), { withFileTypes: true });
    } catch {
      return; // folder doesn't exist
    }
    entries.sort((a, b) => a.name.localeCompare(b.name));
    for (const e of entries) {
      const path = dir === "." ? e.name : `${dir}/${e.name}`;
      // Game logs are .jsonl too, but they hold no positions to draw.
      if (e.isFile() && e.name.endsWith(".jsonl") && !e.name.endsWith(".gamelog.jsonl")) found.push(path);
      if (e.isDirectory() && depth > 0) await scan(path, depth - 1);
    }
  };
  for (const dir of SEARCH) await scan(dir, dir === "out" ? 2 : 0);
  return found;
}

// The Builder is its own page, generate/index.html. Vite's dev server would
// answer /generate with the viewer's index.html, so send it to the folder.
function builderPage(): Plugin {
  return {
    name: "cfbsim-builder-page",
    configureServer(server) {
      server.middlewares.use((req, _res, next) => {
        if (req.url && /^\/generate(\?|$)/.test(req.url)) req.url = req.url.replace("/generate", "/generate/");
        next();
      });
    },
  };
}

// The Play Builder is its own page too (playbuilder/index.html). It reads the
// route, zone, blocking, and QB libraries straight from data/library/ so there
// is one copy: /api/library returns them as one JSON object.
const LIBRARY_FILES = { routes: "routes.json", zones: "zones.json", blocking: "blocking_schemes.json", qb: "qb_actions.json" } as const;

// The library files are JSON with // comment lines.
const parseCommented = (text: string): Record<string, unknown> => JSON.parse(text.replace(/^\s*\/\/.*$/gm, ""));

function playBuilder(): Plugin {
  return {
    name: "cfbsim-play-builder",
    configureServer(server) {
      server.middlewares.use((req, res, next) => {
        // The Route Builder (routebuilder/index.html) is a page of its own too.
        if (req.url && /^\/(playbuilder|routebuilder)(\?|$)/.test(req.url)) req.url = req.url.replace(/^\/(\w+)/, "/$1/");
        if (req.url !== "/api/library") return next();
        Promise.all(Object.values(LIBRARY_FILES).map((f) => readFile(join(REPO, "data", "library", f), "utf8")))
          .then((texts) => {
            const lib: Record<string, unknown> = Object.fromEntries(Object.keys(LIBRARY_FILES).map((k, i) => [k, parseCommented(texts[i]!)]));
            // routes.json and zones.json wrap their table; the page wants the table.
            lib.routes = (lib.routes as { routes: unknown }).routes;
            lib.zones = (lib.zones as { zones: unknown }).zones;
            res.setHeader("Content-Type", "application/json");
            res.setHeader("Cache-Control", "no-store");
            res.end(JSON.stringify(lib));
          })
          .catch((e) => {
            res.statusCode = 500;
            res.end(String(e));
          });
      });
    },
  };
}

export default defineConfig({
  plugins: [trackingFiles(), builderPage(), playBuilder()],
  server: { port: 5173 },
  build: {
    rollupOptions: {
      input: {
        main: fileURLToPath(new URL("index.html", import.meta.url)),
        generate: fileURLToPath(new URL("generate/index.html", import.meta.url)),
        playbuilder: fileURLToPath(new URL("playbuilder/index.html", import.meta.url)),
        routebuilder: fileURLToPath(new URL("routebuilder/index.html", import.meta.url)),
      },
    },
  },
});
