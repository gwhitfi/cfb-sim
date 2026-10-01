import { readdir, readFile } from "node:fs/promises";
import { join, relative, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { defineConfig, type Plugin } from "vite";

// The repo root, where `cfbsim run-drill ... --out x.jsonl` writes by default.
const REPO = fileURLToPath(new URL("..", import.meta.url));
// Folders searched for tracking files (not recursive beyond these).
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
  for (const dir of SEARCH) {
    let names: string[];
    try {
      names = await readdir(join(REPO, dir));
    } catch {
      continue; // folder doesn't exist
    }
    for (const name of names.sort()) {
      if (name.endsWith(".jsonl")) found.push(dir === "." ? name : `${dir}/${name}`);
    }
  }
  return found;
}

export default defineConfig({
  plugins: [trackingFiles()],
  server: { port: 5173 },
});
