import { readdir, readFile } from "node:fs/promises";
import { join, relative, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { defineConfig, type Plugin } from "vite";

// The repo root, where `cfbsim run-drill ... --out x.jsonl` writes by default.
const REPO = fileURLToPath(new URL("..", import.meta.url));
// Folders searched for tracking files: the repo root, out/, and one level of
// folders inside out/ (a game's per-snap files from `--tracking out/NAME`).
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
  for (const dir of SEARCH) await scan(dir, dir === "out" ? 1 : 0);
  return found;
}

export default defineConfig({
  plugins: [trackingFiles()],
  server: { port: 5173 },
});
