// Exercise the actual Live menus and answer payloads in Chromium, using API art.
// node viewer/scripts/check-playmenu.mjs out/defensive-art/playbook.json [offense-art.json]
import assert from 'node:assert/strict';
import { readFile, mkdir } from 'node:fs/promises';
import { resolve } from 'node:path';
import { createServer } from 'vite';
import { chromium } from 'playwright';
const art = JSON.parse(await readFile(process.argv[2], 'utf8'));
assert.equal(art.unit, 'defense');
const offenseArt = process.argv[3] ? JSON.parse(await readFile(process.argv[3], 'utf8')) : null;
const root = resolve('viewer');
const out = resolve('out/defensive-menu');
await mkdir(out, { recursive: true });
const server = await createServer({ root, configFile: false, server: { host: '127.0.0.1', port: 0 } });
let browser;
try {
  await server.listen();
  browser = await chromium.launch({ headless: true });
  const page = await browser.newPage({ viewport: { width: 1440, height: 1000 } });
  const html = (await readFile(`${root}/index.html`, 'utf8')).replace(/<script type="module" src="\/src\/main.ts"><\/script>/, '<link rel="stylesheet" href="/src/style.css">');
  await page.route('**/__menu-check', (route) => route.fulfill({ contentType: 'text/html', body: html }));
  await page.goto(`${server.resolvedUrls.local[0]}__menu-check`);
  const result = await page.evaluate(async (art) => {
    const { Live } = await import('/src/live.ts');
    const ok = (v, message) => { if (!v) throw new Error(message); };
    const equal = (a, b, message) => ok(JSON.stringify(a) === JSON.stringify(b), `${message}: ${JSON.stringify(a)}`);
    const live = new Live({ show: () => true, playback: () => null, devMode: () => false, serverInfo() {} });
    const sent = [];
    live.ws = { readyState: WebSocket.OPEN, send: (s) => sent.push(JSON.parse(s).command) };
    live.mode = 'live';
    window.addEventListener('keydown', (e) => { if (live.key(e)) e.preventDefault(); });
    const state = { tick: 1, quarter: 1, overtime: false, clock: 600, clock_text: '10:00', clock_running: false, play_clock: 30,
      teams: { home: { id: 'h', name: 'Home', abbr: 'HOM' }, away: { id: 'a', name: 'Away', abbr: 'AWY' } },
      score: { home: 0, away: 0 }, timeouts: { home: 3, away: 3 }, offense: 'away', down: 1, distance: 10, goal_to_go: false, yard_line: 25, situation: '1st and 10 at AWY 25', hash: 'middle', offense_right: true };
    const options = art.plays.map((p) => ({ id: p.id, name: p.name, formation: p.formation, formation_name: art.formations.find((f) => f.id === p.formation).name, type: 'defense', flip: false, tags: [] }));
    const [current, alternate, illegal] = options;
    const missing = { ...alternate, id: 'missing', name: 'Missing picture' };
    const prompt = (input = 'call_play', plays = [current, alternate]) => ({ input, team: 'home', live: true, state,
      options: { side: 'defense', plays, play: current.id, offense_personnel: 'OFFENSE SENTINEL', formation_name: 'OFFENSE SENTINEL' }, suggestion: { play: alternate.id } });
    const box = document.getElementById('prompt');
    const buttons = () => [...box.querySelectorAll('button')];
    const click = (text) => { const b = buttons().find((b) => b.textContent.trim() === text); ok(b, `button ${text}`); b.click(); };
    const card = (name) => { const b = [...box.querySelectorAll('.menu-card')].find((b) => b.querySelector('.menu-name')?.textContent === name); ok(b, `card ${name}`); b.click(); };
    const names = () => [...box.querySelectorAll('.menu-name')].map((x) => x.textContent);
    const reset = () => { live.menu = null; live.shownPrompt = null; live.waiting = false; live.answeredLive.clear(); live.early = null; live.held = null; live.earlyShown = false; sent.length = 0; live.art.home.defense = art; };
    const answer = (input, fields) => equal(sent.at(-1), { type: 'answer', input, team: 'home', ...fields }, 'answer payload');
    reset(); live.showPrompt(prompt());
    equal(names(), [art.formations[0].name], 'formation screen');
    card(art.formations[0].name);
    equal(names(), [current.name, alternate.name], 'legal call cards');
    ok(!box.textContent.includes('OFFENSE SENTINEL') && !box.querySelector('input'), 'defense only');
    ok(box.querySelector('.menu-card.suggested .menu-name').textContent === alternate.name, 'suggestion');
    card(alternate.name); answer('call_play', { play: alternate.id });
    reset(); live.showPrompt(prompt('check', [current, alternate, missing]));
    equal(names(), [alternate.name, missing.name], 'check excludes current and illegal');
    ok(!box.textContent.includes(illegal.name) && !box.textContent.includes('OFFENSE SENTINEL'), 'restricted check');
    card(missing.name); answer('check', { play: missing.id });
    reset(); live.showPrompt(prompt('check')); click(`Stay with ${current.name}`); answer('check', { play: current.id });
    reset(); live.showPrompt(prompt('check')); click("Coach's call"); answer('check', { auto: true });
    reset(); live.showPrompt(prompt('check')); card(alternate.name); answer('check', { play: alternate.id });
    reset(); live.showPrompt(prompt('check')); ok(live.key(new KeyboardEvent('keydown', { key: 'Escape' })), 'Escape handled');
    equal(names(), [art.formations[0].name], 'Escape formations');
    live.showPrompt(prompt()); equal(names(), [art.formations[0].name], 'new input resets'); card(art.formations[0].name);
    live.live = [prompt('call_play', [alternate])]; live.showLive(); equal(names(), [alternate.name], 'refresh legal list preserves screen');
    card(alternate.name); answer('call_play', { play: alternate.id });
    reset(); live.showPrompt(prompt('check', [current])); equal(names(), [], 'stay-only check'); click(`Stay with ${current.name}`); answer('check', { play: current.id });
    for (const input of ['call_play', 'check']) {
      reset(); live.art.home.defense = undefined; live.showPrompt(prompt(input));
      ok(!box.querySelector('svg') && !box.textContent.includes('OFFENSE SENTINEL'), 'text fallback');
      card(alternate.name); answer(input, { play: alternate.id });
    }
    const offense = { team: 'h', playbook: 'o', formations: [{ id: 'spread', name: 'Spread', personnel: '11', players: [] }],
      plays: [{ id: 'stick', name: 'Stick', formation: 'spread', type: 'pass', flip: true, routes: [], qb: [], path: null, pulls: [] }] };
    live.art.home.offense = offense;
    const op = { ...prompt(), options: { side: 'offense', plays: [{ ...offense.plays[0], tags: [] }], kick_plays: ['punt', 'field_goal'], clock_plays: ['kneel', 'spike'] } };
    reset(); live.state = state; live.early = 'home'; live.showEarly(); card('Spread');
    box.querySelector('input').click(); card('Stick'); ok(live.held?.fields.flip && sent.length === 0, 'early flipped pick held');
    click('Change'); live.showEarly(); ok(!live.held, 'Change clears held pick'); card('Stick');
    live.live = [op]; live.showLive(); answer('call_play', { play: 'stick', flip: true });
    for (const [id, name] of [['punt', 'Punt'], ['field_goal', 'Field goal'], ['kneel', 'Kneel'], ['spike', 'Spike']]) {
      reset(); live.showPrompt(op); click(name); answer('call_play', { play: id });
    }
    reset(); live.showPrompt({ ...op, options: { ...op.options, kick_plays: ['extra_point'], clock_plays: [] } });
    ok(!box.textContent.includes('Kneel') && !box.textContent.includes('Spike'), 'try clock restrictions');
    click('Extra point'); answer('call_play', { play: 'extra_point' });
    reset(); live.art.home.offense = undefined; live.showPrompt(op); card('Stick'); answer('call_play', { play: 'stick', flip: false });
    reset(); live.showPrompt({ ...prompt(), live: false }); card(art.formations[0].name); card(current.name);
    equal(sent.at(-1), { type: 'answer', play: current.id }, 'blocking answer');
    reset(); live.showPrompt(prompt()); card(art.formations[0].name);
    window.menuTest = { live, prompt, reset, card, click, names, sent, state, offense, op, box, equal, ok };
    return 'Defensive calls/checks, legal refresh, Stay/Coach, Escape, missing/full fallback, and offense early/Change/flip/kick/try regressions passed.';
  }, art);
  await page.locator('#prompt').screenshot({ path: `${out}/call.png` });
  await page.evaluate(() => { window.menuTest.live.menu = null; window.menuTest.live.showPrompt(window.menuTest.prompt('check')); });
  await page.locator('#prompt').screenshot({ path: `${out}/check.png` });
  // Larger books exercise real paging; current data alone is too small.
  await page.evaluate((base) => {
    const t = window.menuTest;
    const { live, reset, card, click, names, equal, ok, box, sent, prompt } = t;
    const fixture = (defense) => {
      const template = defense ? base : t.offense;
      const formations = Array.from({ length: 7 }, (_, i) => ({ ...template.formations[0], id: `f${i}`, name: `Formation ${i}` }));
      const plays = formations.flatMap((f) => Array.from({ length: 7 }, (_, i) => ({ ...template.plays[0], id: `${f.id}p${i}`, name: `${f.name} Play ${i}`, formation: f.id })));
      return { ...template, formations, plays };
    };
    const option = (p) => ({ ...p, type: 'call', flip: true, tags: [] });
    const run = (defense) => {
      reset();
      const art = fixture(defense);
      live.art.home[defense ? 'defense' : 'offense'] = art;
      const missing = Array.from({ length: 4 }, (_, i) => ({ ...option(art.plays[0]), id: `missing${i}`, name: `Missing ${i}` }));
      const p = { ...prompt(), options: { side: defense ? 'defense' : 'offense', plays: [...art.plays.map(option), ...missing] }, suggestion: null };
      live.showPrompt(p);
      equal(names(), ['Formation 0', 'Formation 1', 'Formation 2'], 'three formation cards');
      click('Previous'); equal(names(), ['Missing 2', 'Missing 3'], 'formation wrap to partial page');
      click('Next'); equal(names(), ['Formation 0', 'Formation 1', 'Formation 2'], 'formation wrap to start');
      click('Next'); card('Formation 4');
      equal(names(), ['Formation 4 Play 0', 'Formation 4 Play 1', 'Formation 4 Play 2'], 'new formation page one');
      click('Next'); equal(names(), ['Formation 4 Play 3', 'Formation 4 Play 4', 'Formation 4 Play 5'], 'play page two');
      click('‹ Formations (Esc)'); equal(names(), ['Formation 3', 'Formation 4', 'Formation 5'], 'back preserves formation page');
      card('Formation 4');
      click('Previous'); equal(names(), ['Missing 2', 'Missing 3'], 'play wrap to partial placeholders');
      click('Next'); equal(names()[0], 'Formation 4 Play 0', 'play wrap to start');
      const updated = { ...p, suggestion: { play: 'f6p5', flip: !defense } };
      live.live = [updated]; live.showLive();
      equal(names()[0], 'Formation 4 Play 0', 'suggestion arrival does not move page');
      box.querySelector('.menu-pages button').focus();
      box.closest('.prompt').querySelector('.prompt-head .coach').focus();
      live.live = [{ ...updated, suggestion: { ...updated.suggestion, focusRefresh: true } }]; live.showLive();
      ok(document.activeElement.matches('.prompt-head .coach'), 'refresh preserves Coach action focus');
      click('Show suggestion');
      equal(names(), ['Formation 6 Play 3', 'Formation 6 Play 4', 'Formation 6 Play 5'], 'suggestion page');
      equal(document.activeElement.dataset.cardId, 'play:f6p5', 'suggestion focuses card');
      if (!defense) ok(box.querySelector('input').checked, 'suggested flip');
      ok(sent.length === 0, 'browsing and suggestions never answer');
      live.live = [{ ...updated, options: { ...updated.options, plays: updated.options.plays.filter((x) => x.id !== 'missing3') } }];
      live.showLive();
      equal(document.activeElement.dataset.cardId, 'play:f6p5', 'refresh preserves focus');
      equal(names()[0], 'Formation 6 Play 3', 'refresh preserves page');
      return { p: updated, art };
    };
    const offenseRun = run(false);
    reset(); live.art.home.offense = offenseRun.art; live.state = t.state; live.early = 'home'; live.showEarly();
    click('Next'); card('Formation 4'); click('Next');
    box.querySelector('input').click();
    live.live = [{ ...offenseRun.p, suggestion: null }]; live.showLive();
    equal(names()[0], 'Formation 4 Play 3', 'early menu keeps formation and page when call opens');
    ok(box.querySelector('input').checked, 'early menu keeps flip');
    ok(sent.length === 0, 'early browsing does not answer');
    reset(); live.showPrompt({ ...t.op, suggestion: { play: 'punt' } }); click('Show suggestion');
    equal(document.activeElement.textContent, 'Punt', 'action suggestion focuses action');
    ok(sent.length === 0, 'action suggestion does not answer');
    const { p, art } = run(true);
    Object.assign(t, { p, art });
  }, art);
  // Real keyboard input exercises focus, native event targets, and activation.
  await page.keyboard.press('ArrowRight');
  assert.deepEqual(await page.locator('.menu-name').allTextContents(), ['Formation 6 Play 6', 'Missing 0', 'Missing 1']);
  await page.keyboard.press('ArrowLeft');
  assert.equal(await page.evaluate(() => document.activeElement.dataset.cardId), 'play:f6p5');
  await page.keyboard.press('Escape');
  assert.deepEqual(await page.locator('.menu-name').allTextContents(), ['Formation 3', 'Formation 4', 'Formation 5']);
  await page.keyboard.press('ArrowRight');
  await page.keyboard.press('Enter'); // focused formation, not a call
  assert.equal(await page.evaluate(() => window.menuTest.sent.length), 0);
  await page.keyboard.press('Space'); // focused play
  assert.deepEqual(await page.evaluate(() => window.menuTest.sent.at(-1)), { type: 'answer', input: 'call_play', team: 'home', play: 'f5p0' });
  await page.evaluate(() => {
    const { live, p, reset, art, names, equal, click } = window.menuTest;
    reset(); live.art.home.defense = art; live.showPrompt(p);
    equal(names()[0], 'Formation 0', 'answer resets next decision');
    click('Next');
    // A server-closed decision with no user answer also resets its page.
    live.live = []; live.showLive(); live.live = [p]; live.showLive();
    equal(names()[0], 'Formation 0', 'expired decision resets');
    click('Show suggestion');
    const input = document.createElement('textarea'); input.id = 'typing'; document.body.append(input); input.focus();
  });
  const beforeTyping = await page.locator('.menu-name').allTextContents();
  await page.keyboard.press('ArrowLeft'); await page.keyboard.press('Escape'); await page.keyboard.press('Space');
  assert.deepEqual(await page.locator('.menu-name').allTextContents(), beforeTyping);
  assert.equal(await page.evaluate(() => window.menuTest.sent.length), 0);
  await page.locator('#typing').evaluate((e) => e.remove());
  // ResizeObserver responds to available panel width, including sidebar changes.
  for (const [width, size] of [[900, 3], [650, 2], [420, 1], [900, 3]]) {
    const anchor = (await page.locator('.menu-card').evaluateAll((els) => els.map((e) => e.dataset.cardId)))[0];
    await page.locator('#prompt').evaluate((e, width) => { e.style.width = `${width}px`; e.style.right = 'auto'; }, width);
    await page.waitForFunction((size) => document.querySelector('.menu-grid').style.gridTemplateColumns.startsWith(`repeat(${size},`), size);
    const ids = await page.locator('.menu-card').evaluateAll((els) => els.map((e) => e.dataset.cardId));
    assert.ok(ids.length <= size && ids.includes(anchor), 'resize retains anchor');
    await page.locator('#prompt').screenshot({ path: `${out}/paged-${size}.png` });
  }
  await page.evaluate(() => {
    const { live, p, names, equal, ok, box, sent, click, reset, art } = window.menuTest;
    live.live = [{ ...p, options: { ...p.options, plays: p.options.plays.filter((x) => x.id === 'f6p0') } }]; live.showLive();
    equal(names(), ['Formation 6 Play 0'], 'removed pages clamp');
    ok([...box.querySelectorAll('.menu-pages button')].every((b) => b.disabled), 'single page disables cycling');
    live.live = [{ ...p, options: { ...p.options, plays: [] } }]; live.showLive();
    equal(names(), [], 'empty list');
    ok(box.textContent.includes('Page 0 of 0'), 'empty page count');
    ok([...box.querySelectorAll('.menu-pages button')].every((b) => b.disabled), 'empty disables cycling');
    reset(); live.art.home.defense = undefined; live.showPrompt(p);
    ok(box.querySelectorAll('.menu-card').length === 3 && !box.querySelector('svg'), 'full fallback paged');
    click('Next'); ok(box.querySelectorAll('.menu-card').length === 3, 'fallback next page');
    click('Show suggestion'); equal(document.activeElement.dataset.cardId, 'play:f6p5', 'fallback suggestion navigation');
    ok(sent.length === 0, 'fallback browsing never submits');
    reset(); live.art.home.defense = art; live.showPrompt(p); click('Show suggestion');
    live.showPrompt({ ...p, input: 'check', options: { ...p.options, play: 'f0p0' } });
    equal(names()[0], 'Formation 0', 'check resets call page');
    live.art.away.defense = art; live.showPrompt({ ...p, team: 'away' });
    equal(names()[0], 'Formation 0', 'side change resets page');
  });
  if (offenseArt) {
    await page.evaluate(({ offense, defense }) => {
      const t = window.menuTest;
      t.realOffense = offense; t.realDefense = defense;
      t.box.style.width = ''; t.box.style.right = '';
      t.live.user = new Set(['home']);
      document.getElementById('game').hidden = false;
      t.live.showMine(true);
      t.visual = (mode) => {
        t.reset(); t.live.resetMenu();
        t.live.art.home.offense = offense; t.live.art.home.defense = defense;
        const isOffense = mode.startsWith('offense') || ['early', 'held'].includes(mode);
        const book = isOffense ? offense : defense;
        const state = { ...t.state, offense: isOffense ? 'home' : 'away' };
        t.live.state = state; t.live.scoreboard(state);
        const plays = book.plays.map((p) => ({ ...p, type: 'call', flip: !!p.flip, tags: [] }));
        const p = { input: mode === 'check' ? 'check' : 'call_play', team: 'home', live: true, state,
          options: { side: isOffense ? 'offense' : 'defense', plays, play: plays[0].id,
            kick_plays: isOffense ? ['punt', 'field_goal'] : [], clock_plays: isOffense ? ['kneel', 'spike'] : [] },
          suggestion: { play: plays[1].id } };
        if (mode.endsWith('fallback')) t.live.art.home[isOffense ? 'offense' : 'defense'] = undefined;
        if (['early', 'held'].includes(mode)) {
          t.live.early = 'home'; t.live.showEarly();
          t.card(book.formations[0].name);
          if (mode === 'held') t.card(t.names()[0]);
        } else {
          t.live.showPrompt(p);
          if (!mode.endsWith('formations') && !mode.endsWith('fallback')) t.click('Show suggestion');
        }
        document.getElementById('sb-play-clock').textContent = ':30';
        t.box.scrollTop = 0;
        return { sent: t.sent.length, isOffense };
      };
    }, { offense: offenseArt, defense: art });
    for (const [label, width, height] of [['desktop', 1440, 900], ['medium', 1100, 900], ['narrow', 390, 844], ['short', 1024, 520]]) {
      await page.setViewportSize({ width, height });
      for (const mode of ['offense-formations', 'defense-formations', 'offense', 'defense', 'check', 'early', 'held', 'offense-fallback', 'defense-fallback']) {
        const initial = await page.evaluate((mode) => window.menuTest.visual(mode), mode);
        await page.waitForFunction(() => {
          const body = document.querySelector('#prompt .prompt-body');
          if (!body) return true;
          const size = body.clientWidth >= 840 ? 3 : body.clientWidth >= 560 ? 2 : 1;
          return body.querySelector('.menu-grid').style.gridTemplateColumns.startsWith(`repeat(${size},`);
        });
        const metrics = await page.evaluate(async ({ mode, initial }) => {
          const t = window.menuTest;
          const { box, ok, equal } = t;
          const panel = box.getBoundingClientRect();
          const board = document.getElementById('scoreboard').getBoundingClientRect();
          const stage = document.querySelector('.stage').getBoundingClientRect();
          ok(panel.top >= board.bottom + 3, `${mode}: scoreboard overlap ${panel.top} / ${board.bottom}`);
          ok(panel.bottom <= stage.bottom && panel.left >= 0 && panel.right <= innerWidth, `${mode}: panel bounds`);
          ok(board.left >= 0 && board.right <= innerWidth, `${mode}: scoreboard fits`);
          ok(box.scrollWidth <= box.clientWidth + 1, `${mode}: horizontal overflow`);
          equal(t.sent.length, initial.sent, 'layout and browsing never answer');
          const cards = [...box.querySelectorAll('.menu-card')];
          if (cards.length) {
            const body = box.querySelector('.prompt-body');
            const size = body.clientWidth >= 840 ? 3 : body.clientWidth >= 560 ? 2 : 1;
            ok(cards.length <= size, `${mode}: card count`);
            const tops = cards.map((c) => c.getBoundingClientRect().top);
            ok(tops.every((top) => Math.abs(top - tops[0]) < 1), `${mode}: single row`);
            // Compare the inserted art to the unchanged pure renderer for this call.
            const { formationSvg, playSvg, defensiveFormationSvg, defensivePlaySvg } = await import('/src/art.ts');
            const book = initial.isOffense ? t.realOffense : t.realDefense;
            for (const c of cards) {
              if (!c.querySelector('svg')) continue;
              const [kind, id] = c.dataset.cardId.split(':');
              const f = book.formations.find((f) => f.id === id);
              const p = book.plays.find((p) => p.id === id);
              const svg = kind === 'formation' ? (initial.isOffense ? formationSvg(f) : defensiveFormationSvg(f)) :
                initial.isOffense ? playSvg(p, book.formations.find((f) => f.id === p.formation)) : defensivePlaySvg(p);
              const expected = document.createElement('div'); expected.innerHTML = svg;
              equal(c.querySelector('svg').outerHTML, expected.firstElementChild.outerHTML, 'correct unit and unchanged art');
            }
          }
          return { cards: cards.length, scroll: box.scrollHeight > box.clientHeight };
        }, { mode, initial });
        // Every enabled action/card remains reachable through internal scrolling.
        const controls = page.locator('#prompt button:enabled, #prompt input');
        for (let i = 0; i < await controls.count(); i++) {
          const control = controls.nth(i);
          await control.scrollIntoViewIfNeeded();
          assert.ok(await control.isVisible(), `${label}/${mode}: visible control`);
          await control.evaluate((e) => {
            const r = e.getBoundingClientRect(); const p = e.closest('#prompt').getBoundingClientRect();
            const y = Math.max(r.top, p.top) + Math.min(r.height, p.height) / 2;
            if (!e.contains(document.elementFromPoint(r.left + r.width / 2, Math.min(y, p.bottom - 2)))) throw new Error('Control obscured');
          });
        }
        await page.locator('#game-timeout').scrollIntoViewIfNeeded();
        assert.ok(await page.locator('#game-timeout').isVisible(), 'timeout remains reachable');
        await page.evaluate(() => { window.menuTest.box.scrollTop = 0; });
        await page.screenshot({ path: `${out}/${label}-${mode}.png` });
        console.log(`Visual ${label}/${mode}: ${metrics.cards} cards${metrics.scroll ? ', internal scrolling' : ''}`);
      }
    }
    await page.evaluate(() => { const t = window.menuTest; t.visual('defense'); t.sent.length = 0; });
    await page.locator('#game-timeout').click();
    assert.deepEqual(await page.evaluate(() => window.menuTest.sent.at(-1)), { type: 'timeout', team: 'home' });
  }
  console.log('Paging: both units, oversized books, partial/empty/single pages, wraparound, keyboard activation, focus, suggestions, resize, legal refresh, fallback, and decision reset passed.');
  console.log(result);
  console.log(`Screenshots: ${out}`);
} finally {
  await browser?.close();
  await server.close();
}
