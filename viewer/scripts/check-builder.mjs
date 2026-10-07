// Review builder import/edit/export and responsive inspector layout.
// node viewer/scripts/check-builder.mjs
import assert from 'node:assert/strict';
import { readFile, mkdir } from 'node:fs/promises';
import { resolve } from 'node:path';
import { createServer } from 'vite';
import { chromium } from 'playwright';
const book = JSON.parse(await readFile('data/playbooks/43_base.json', 'utf8'));
const quarters = book.plays.find((p) => p.id === 'cover_4_quarters');
const out = resolve('out/builder-cleanup');
await mkdir(out, { recursive: true });
const server = await createServer({ root: resolve('viewer'), configFile: resolve('viewer/vite.config.ts'), server: { host: '127.0.0.1', port: 0 } });
let browser; let page;
try {
  await server.listen();
  browser = await chromium.launch({ headless: true });
  page = await browser.newPage({ viewport: { width: 1500, height: 1000 } });
  page.setDefaultTimeout(10000);
  const errors = []; page.on('pageerror', (e) => errors.push(e.message));
  await page.goto(`${server.resolvedUrls.local[0]}playbuilder/`);
  await page.locator('#output textarea').first().waitFor();
  await page.locator('#btn-defense').click();
  await page.locator('#btn-import').click();
  await page.locator('#import-text').fill(JSON.stringify(quarters));
  await page.locator('#import-go').click();
  const output = async () => page.locator('#output textarea').evaluateAll((els) => els.map((e) => { try { return JSON.parse(e.value); } catch { return null; } }).find((x) => x?.assignments));
  assert.deepEqual(await output(), quarters, 'import/export preserves all assignments');
  await page.locator('#field [data-slot="FS"]').first().click();
  await page.locator('#right label').filter({ hasText: /^Zone read/ }).locator('select').selectOption('#2_weak');
  assert.equal((await output()).assignments.FS.pass.read, '#2_weak');
  await page.reload();
  await page.locator('#right label').filter({ hasText: /^Zone read/ }).locator('select').waitFor();
  assert.equal(await page.locator('#right label').filter({ hasText: /^Zone read/ }).locator('select').inputValue(), '#2_weak', 'draft reload preserves read');
  await page.locator('#right label').filter({ hasText: /^Zone read/ }).locator('select').selectOption('');
  assert.equal((await output()).assignments.FS.pass.read, undefined, 'Automatic removes override');
  assert.equal((await output()).assignments.SS.pass.read, '#2_strong', 'other assignments remain intact');
  for (const [name, width, height] of [['desktop', 1500, 1000], ['narrow', 390, 844]]) {
    await page.setViewportSize({ width, height });
    await page.locator('#right label').filter({ hasText: /^Zone read/ }).locator('select').scrollIntoViewIfNeeded();
    const overflow = await page.evaluate(() => document.documentElement.scrollWidth > innerWidth);
    assert.equal(overflow, false, `${name}: no horizontal overflow`);
    await page.screenshot({ path: `${out}/${name}.png`, fullPage: name === 'narrow' });
  }
  assert.deepEqual(errors, []);
  console.log('PASS: real builder import/edit/export, draft reload, clear read, desktop/narrow layout');
} catch (e) { if (page) { console.error(await page.locator('#right').innerText()); await page.screenshot({ path: `${out}/failure.png` }); } throw e; } finally { await browser?.close(); await server.close(); }
