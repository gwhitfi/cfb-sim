// Review base defensive SVGs with the actual viewer CSS at three card sizes.
// node --experimental-strip-types viewer/scripts/defense-art.mjs ART.json [OUT]
// ART.json is the payload from cfbsim_defensive_playbook_art.
import assert from "node:assert/strict";
import { readFile, mkdir, writeFile } from "node:fs/promises";
import { resolve, join } from "node:path";
import { chromium } from "playwright";
import { defensiveFormationSvg, defensivePlaySvg } from "../src/art.ts";

if (!process.argv[2]) throw new Error("usage: defense-art.mjs ART.json [OUT]");
const art = JSON.parse(await readFile(resolve(process.argv[2]), "utf8"));
assert.equal(art.unit, "defense");
const out = resolve(process.argv[3] ?? "out/defensive-art");
await mkdir(out, { recursive: true });
const css = await readFile(new URL("../src/style.css", import.meta.url), "utf8");
const esc = (s) => s.replaceAll("&", "&amp;").replaceAll("<", "&lt;").replaceAll('"', "&quot;");
const cards = [
  ...art.formations.map((f) => ({ name: f.name, detail: f.package, svg: defensiveFormationSvg(f) })),
  ...art.plays.map((p) => ({ name: p.name, detail: p.coverage, svg: defensivePlaySvg(p) })),
];
// Exercise spy/stunt display even when the current playbook has neither.
const sample = structuredClone(art.plays[0]);
const spy = sample.players.find((p) => p.role === "lb");
const stunt = sample.jobs.find((j) => j.kind === "rush");
if (spy && stunt) {
  sample.name = "Spy and stunt (renderer sample)";
  sample.jobs = sample.jobs.filter((j) => j.slot !== spy.slot);
  sample.jobs.push({ slot: spy.slot, kind: "spy", at: [0, 5] });
  stunt.kind = "stunt";
  cards.push({ name: sample.name, detail: "Synthetic job coverage", svg: defensivePlaySvg(sample) });
}
const browser = await chromium.launch({ headless: true });
try {
  const page = await browser.newPage({ viewport: { width: 1120, height: 900 }, deviceScaleFactor: 1 });
  for (const width of [320, 260, 210]) {
    const html = `<!doctype html><html><head><meta charset="utf-8"><title>Defensive base cards</title>
      <style>${css}
      html, body { display:block; height:auto; overflow:visible; margin:0; background:#10151d; color:#f1f5f9; }
      body { padding:24px; font:14px system-ui,sans-serif; }
      h1 { font-size:22px; margin:0 0 8px; }
      p { color:#aebdcd; margin:0 0 20px; }
      main { display:grid; grid-template-columns:repeat(3, ${width}px); gap:16px; }
      article { border:1px solid #35404e; border-radius:8px; overflow:hidden; background:#18202b; }
      h2 { font-size:14px; padding:10px 12px 4px; margin:0; }
      article p { font-size:12px; padding:0 12px 12px; margin:0; }
      </style></head><body><h1>Defensive base calls · ${width}px cards</h1>
      <p>Blue / green: zones · Gold ring: man · Violet diamond: spy · Red dashed arrow: blitz</p>
      <main>${cards.map((c) => `<article>${c.svg}<h2>${esc(c.name)}</h2><p>${esc(c.detail)}</p></article>`).join("")}</main>
      </body></html>`;
    await page.setContent(html);
    await page.evaluate(() => document.fonts.ready);
    const failures = await page.evaluate(() => {
      const failures = [];
      for (const svg of document.querySelectorAll(".art-defense")) {
        if (getComputedStyle(svg).overflow !== "hidden") failures.push("unclipped SVG");
        if (svg.querySelectorAll(".art-player").length !== 11) failures.push("not eleven defenders");
        if (svg.querySelectorAll(".art-defender-label").length !== 11) failures.push("missing slot labels");
        const bounds = svg.getBoundingClientRect();
        const labels = [...svg.querySelectorAll(".art-defender-label")];
        for (let i = 0; i < labels.length; i++) {
          const a = labels[i].getBoundingClientRect();
          if (a.top < bounds.top || a.bottom > bounds.bottom || a.left < bounds.left || a.right > bounds.right) {
            failures.push(`clipped label ${labels[i].textContent}`);
          }
          for (const other of labels.slice(i + 1)) {
            const b = other.getBoundingClientRect();
            if (a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top) {
              failures.push(`overlapping labels ${labels[i].textContent}/${other.textContent}`);
            }
          }
        }
      }
      return failures;
    });
    await writeFile(join(out, `defense-${width}.html`), html);
    await page.screenshot({ path: join(out, `defense-${width}.png`), fullPage: true });
    assert.deepEqual(failures, [], `${width}px cards`);
    console.log(`PASS: ${cards.length} defensive cards at ${width}px; wrote HTML and PNG`);
  }
} finally {
  await browser.close();
}
