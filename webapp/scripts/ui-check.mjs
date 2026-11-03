#!/usr/bin/env node
/**
 * ui-check — layout / a11y / motion acceptance run (review 2026-10-13 wave 4; spec §10.2/10.3/
 * 10.5/10.6) over Chrome DevTools Protocol. No new dependencies: Node ≥ 22 (global WebSocket),
 * axe-core from node_modules, PNG comparison done inside the browser (canvas).
 *
 * Needs a Chrome with --remote-debugging-port (CDP, default http://localhost:9222) and the app
 * (default http://localhost:3737/, demo mode). Opens its OWN tabs and closes them.
 *   node scripts/ui-check.mjs                 full matrix → reports/ui-check/report.{json,md}
 *   node scripts/ui-check.mjs --quick         390 + 1440 only
 *   node scripts/ui-check.mjs --update        (re)write screenshot baselines
 * Env: APP, CDP, OUT (report dir), BASELINE (baseline dir), ONLY (route ids, comma list, e.g. ONLY=home,config,point — «home» includes home-analytics).
 * Exit code 1 when any hard check fails (hscroll, text < 11 px, axe serious/critical, §10.x).
 */
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const APP = process.env.APP || 'http://localhost:3737/';
const CDP = process.env.CDP || 'http://localhost:9222';
const OUT = path.resolve(process.env.OUT || path.join(ROOT, 'reports', 'ui-check'));
const BASELINE = path.resolve(process.env.BASELINE || path.join(ROOT, 'reports', 'ui-baseline'));
const QUICK = process.argv.includes('--quick');
const UPDATE = process.argv.includes('--update');
const ONLY = (process.env.ONLY || '').split(',').map((x) => x.trim()).filter(Boolean);
const MIN_PX = 11;
const DIFF_FAIL = 0.06; // > 6 % of pixels changed vs baseline = layout regression (live numbers ≈ 1–3 %)
const AXE_SRC = fs.readFileSync(path.join(ROOT, 'node_modules', 'axe-core', 'axe.min.js'), 'utf8');

const WIDTHS = QUICK ? [390, 1440] : [360, 390, 820, 1100, 1440];
const ROUTES = [
  { id: 'home', hash: '/dashboard', scenario: 'home' },
  { id: 'home-analytics', hash: '/dashboard', scenario: 'home', click: ['Аналитика', 'Analytics'] },
  { id: 'industrial', hash: '/dashboard', scenario: 'industrial' },
  // US split-phase demo (120/240 V, 60 Hz, USD; rbgrid.demo.region = us): the legs card + nominals.
  { id: 'home-us', hash: '/dashboard', scenario: 'home', region: 'us' },
  { id: 'industrial-us', hash: '/dashboard', scenario: 'industrial', region: 'us' },
  { id: 'ev', hash: '/dashboard', scenario: 'ev' },
  { id: 'point', hash: '/point/9020' },
  { id: 'devices', hash: '/devices' },
  { id: 'history', hash: '/history' },
  { id: 'events', hash: '/events' },
  { id: 'economics', hash: '/economics' },
  { id: 'tariffs', hash: '/tariffs' },
  { id: 'config', hash: '/config' },
  { id: 'onboarding', hash: '/onboarding' },
];

// No generation configured (spec §10.3): the solar unit becomes an ordinary load.
const NO_GEN_UNITS = 'NO_GEN';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function openTab() {
  const tab = await (await fetch(`${CDP}/json/new?about:blank`, { method: 'PUT' })).json();
  const ws = new WebSocket(tab.webSocketDebuggerUrl);
  await new Promise((r, j) => { ws.addEventListener('open', r, { once: true }); ws.addEventListener('error', j, { once: true }); });
  let id = 0;
  const pending = new Map();
  const errors = [];
  ws.addEventListener('message', (e) => {
    const m = JSON.parse(e.data);
    if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); return; }
    if (m.method === 'Runtime.exceptionThrown') errors.push(m.params.exceptionDetails.exception?.description ?? m.params.exceptionDetails.text);
  });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pending.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
  const evaluate = async (expression) => {
    const r = await send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true });
    if (r.result?.exceptionDetails) throw new Error(r.result.exceptionDetails.exception?.description ?? 'eval failed');
    return r.result?.result?.value;
  };
  await send('Page.enable');
  await send('Runtime.enable');
  return { send, evaluate, errors, close: () => fetch(`${CDP}/json/close/${tab.id}`) };
}

/** Seed localStorage for a case (demo connection, theme, language, scenario, units) and load. */
async function load(t, c) {
  await t.send('Emulation.setDeviceMetricsOverride', { width: c.width, height: 900, deviceScaleFactor: 1, mobile: c.width < 820 });
  await t.send('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-reduced-motion', value: c.reduce ? 'reduce' : 'no-preference' }] });
  await t.send('Page.navigate', { url: APP });
  await sleep(1200);
  const demo = { system: { site_profile: c.scenario || 'home' } };
  await t.evaluate(`(() => {
    localStorage.setItem('rbgrid.connection', JSON.stringify({ baseUrl: '', demo: true }));
    localStorage.setItem('rbgrid.theme', ${JSON.stringify(c.theme)});
    localStorage.setItem('rbgrid.lang', ${JSON.stringify(c.lang)});
    localStorage.setItem('rbgrid.demo.config', ${JSON.stringify(JSON.stringify(demo))});
    localStorage.setItem('rbgrid.demo.noGen', ${JSON.stringify(c.units === NO_GEN_UNITS ? '1' : '')});
    localStorage.setItem('rbgrid.demo.region', ${JSON.stringify(c.region || '')});
  })()`);
  // A NEW document (query busts same-document hash navigation) at the target route, then wait
  // until it is really ready — deterministic screenshots, no «Загрузка…» frames in baselines.
  await t.send('Page.navigate', { url: `${APP}?uc=${Date.now()}#${c.hash}` });
  await waitReady(t, c.hash);
  for (const label of c.click ?? []) {
    const ok = await t.evaluate(`(() => { const b=[...document.querySelectorAll('button')].find(x=>x.textContent.trim()===${JSON.stringify(label)}); if(b){b.click();return true} return false })()`);
    if (ok) { await sleep(1500); break; }
  }
}

/** Ready = route rendered (hash matches), fonts loaded, no spinners / loading text, then settle. */
async function waitReady(t, hash) {
  const deadline = Date.now() + 15000;
  while (Date.now() < deadline) {
    await sleep(400);
    const ok = await t.evaluate(`(() => {
      if (document.readyState !== 'complete' || location.hash !== ${JSON.stringify('#' + hash)}) return false;
      if (document.fonts && document.fonts.status !== 'loaded') return false;
      if (document.querySelector('.MuiCircularProgress-root, .MuiSkeleton-root')) return false;
      const txt = document.body.innerText || '';
      return txt.length > 200 && !/Загрузка|Loading/.test(txt);
    })()`).catch(() => false);
    if (ok) break;
  }
  await sleep(1500); // live values settle; chart enter-transitions finish
}

const PROBE = `(() => {
  const out = { hscroll: document.documentElement.scrollWidth > window.innerWidth + 1, tiny: [], clipped: [], overlap: 0 };
  const vis = (el) => { const r = el.getBoundingClientRect(); const s = getComputedStyle(el);
    return r.width > 0 && r.height > 0 && s.visibility !== 'hidden' && s.display !== 'none' && +s.opacity !== 0; };
  const label = (el) => (el.textContent || '').trim().replace(/\\s+/g, ' ').slice(0, 40);
  const all = document.querySelectorAll('body *');
  for (const el of all) {
    if (['SCRIPT','STYLE','svg','defs','title'].includes(el.tagName)) continue;
    const own = [...el.childNodes].some((n) => n.nodeType === 3 && n.textContent.trim());
    if (!own || !vis(el)) continue;
    const s = getComputedStyle(el);
    let px = parseFloat(s.fontSize);
    if (el instanceof SVGElement && el.getScreenCTM) { const m = el.getScreenCTM(); if (m) px *= Math.hypot(m.a, m.b); }
    if (px < ${MIN_PX} - 0.05) out.tiny.push({ px: Math.round(px * 10) / 10, text: label(el) });
    if (!(el instanceof SVGElement) && (s.overflowX === 'hidden' || s.overflow === 'hidden') && s.textOverflow !== 'ellipsis'
        && el.scrollWidth > el.clientWidth + 2) out.clipped.push(label(el));
  }
  out.tiny = out.tiny.slice(0, 15); out.clipped = out.clipped.slice(0, 15);
  return out;
})()`;

const AXE_RUN = `(async () => {
  const r = await axe.run(document, { runOnly: { type: 'tag', values: ['wcag2a', 'wcag2aa'] }, resultTypes: ['violations'] });
  return r.violations.map((v) => ({ id: v.id, impact: v.impact, n: v.nodes.length,
    target: v.nodes.slice(0, 3).map((x) => x.target.join(' ') + (x.any[0]?.data?.contrastRatio ? ' ' + x.any[0].data.contrastRatio + ':1' : '')),
    html: v.nodes.slice(0, 2).map((x) => x.html.slice(0, 160)) }));
})()`;

async function screenshot(t) {
  const h = await t.evaluate('Math.min(Math.max(document.documentElement.scrollHeight, document.body.scrollHeight), 8000)');
  const w = await t.evaluate('window.innerWidth');
  const r = await t.send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: true, clip: { x: 0, y: 0, width: w, height: h, scale: 1 } });
  return r.result.data;
}

/** Share of differing pixels between two PNGs (base64), computed in the page via canvas. */
async function diffPng(t, a, b) {
  return t.evaluate(`(async () => {
    const img = (s) => new Promise((ok, no) => { const i = new Image(); i.onload = () => ok(i); i.onerror = no; i.src = 'data:image/png;base64,' + s; });
    const [A, B] = await Promise.all([img(${JSON.stringify(a)}), img(${JSON.stringify(b)})]);
    if (A.width !== B.width) return 1;
    const w = A.width, h = Math.max(A.height, B.height);
    const px = (I) => { const c = document.createElement('canvas'); c.width = w; c.height = h; const x = c.getContext('2d'); x.drawImage(I, 0, 0); return x.getImageData(0, 0, w, h).data; };
    const p = px(A), q = px(B); let d = 0;
    for (let i = 0; i < p.length; i += 4) if (Math.abs(p[i]-q[i]) + Math.abs(p[i+1]-q[i+1]) + Math.abs(p[i+2]-q[i+2]) > 48) d++;
    return d / (w * h);
  })()`);
}

function cases() {
  const list = [];
  for (const r of ROUTES) for (const width of WIDTHS) {
    list.push({ ...r, width, theme: 'dark', lang: 'ru', key: `${r.id}_${width}_dark_ru`, axe: width === 390 || width === 1440, shot: true });
    if (width === 390 || width === 1440) list.push({ ...r, width, theme: 'light', lang: 'ru', key: `${r.id}_${width}_light_ru`, axe: true });
    if (width === 360 || width === 1440) list.push({ ...r, width, theme: 'dark', lang: 'en', key: `${r.id}_${width}_dark_en` });
  }
  // Match by ROUTE id (the key's first segment), not substring: «ev» must not pull devices/events;
  // «home» also takes its views («home-analytics»).
  const route = (k) => k.split('_')[0];
  return list.filter((c) => !ONLY.length || ONLY.some((o) => route(c.key) === o || route(c.key).startsWith(o + '-')));
}

async function main() {
  fs.mkdirSync(path.join(OUT, 'shots'), { recursive: true });
  if (UPDATE) fs.mkdirSync(BASELINE, { recursive: true });
  const results = [];
  const t = await openTab();
  try {
    for (const c of cases()) {
      await load(t, c);
      const probe = await t.evaluate(PROBE);
      const res = { key: c.key, ...probe, axe: [], diff: null, errors: [] };
      if (c.axe) { await t.evaluate(AXE_SRC + ';0'); res.axe = await t.evaluate(AXE_RUN); }
      if (c.shot) {
        const png = await screenshot(t);
        fs.writeFileSync(path.join(OUT, 'shots', `${c.key}.png`), Buffer.from(png, 'base64'));
        const bl = path.join(BASELINE, `${c.key}.png`);
        if (UPDATE) fs.writeFileSync(bl, Buffer.from(png, 'base64'));
        else if (fs.existsSync(bl)) res.diff = await diffPng(t, fs.readFileSync(bl).toString('base64'), png);
      }
      res.errors = t.errors.splice(0);
      results.push(res);
      process.stdout.write(`${c.key.padEnd(34)} hscroll=${probe.hscroll ? 'YES' : 'no'} tiny=${probe.tiny.length} axe=${res.axe.length}${res.diff != null ? ` diff=${(res.diff * 100).toFixed(1)}%` : ''}\n`);
    }

    // §10.5 — prefers-reduced-motion: nothing moves (home schema dots, CSS animations, tweens).
    const motion = [];
    for (const sc of ['home', 'industrial', 'ev']) {
      await load(t, { hash: '/dashboard', scenario: sc, width: 1440, theme: 'dark', lang: 'ru', reduce: true });
      await sleep(1500);
      motion.push({ scenario: sc, ...(await t.evaluate(`({ smil: document.querySelectorAll('animateMotion, animate, animateTransform').length,
        running: document.getAnimations().filter((a) => a.playState === 'running' && (a.effect?.getTiming?.().iterations === Infinity)).length })`)) });
    }

    // §10.2 — 20 s of live data: no schema SVG is recreated (in-place update only).
    const stability = [];
    for (const sc of ['home', 'industrial', 'ev']) {
      for (const click of [[], ['Аналитика']]) {
        if (sc !== 'home' && click.length) continue;
        await load(t, { hash: '/dashboard', scenario: sc, width: 1440, theme: 'dark', lang: 'ru', click });
        await t.evaluate(`(() => { window.__svgs = [...document.querySelectorAll('main svg, [role=main] svg, svg.fl')].filter((s) => s.getBoundingClientRect().width > 200); window.__svgs.forEach((s) => s.dataset.uc = '1'); })()`);
        await sleep(20000);
        stability.push({ view: sc + (click.length ? '-analytics' : ''), ...(await t.evaluate(`({ tracked: window.__svgs.length,
          recreated: window.__svgs.filter((s) => !s.isConnected).length })`)) });
      }
    }

    // §10.3 — home without generation: no word about solar / export / generation share.
    await load(t, { hash: '/dashboard', scenario: 'home', width: 1440, theme: 'dark', lang: 'ru', units: NO_GEN_UNITS });
    const noGenText = await t.evaluate(`(document.body.innerText.match(/солн\\S*|отда\\S*|генерац\\S*|от солнца/gi) || []).slice(0, 10)`);
    await t.evaluate(`localStorage.setItem('rbgrid.demo.noGen', ''); localStorage.removeItem('rbgrid.demo.region'); localStorage.removeItem('rbgrid.demo.config'); localStorage.setItem('rbgrid.theme', 'dark'); localStorage.setItem('rbgrid.lang', 'ru')`);

    const fail = [];
    for (const r of results) {
      if (r.hscroll) fail.push(`${r.key}: horizontal scroll`);
      if (r.tiny.length) fail.push(`${r.key}: text < ${MIN_PX}px ×${r.tiny.length} (${r.tiny.slice(0, 3).map((x) => `${x.px}px «${x.text}»`).join(', ')})`);
      for (const v of r.axe) if (v.impact === 'serious' || v.impact === 'critical') fail.push(`${r.key}: axe ${v.id} ×${v.n} (${v.target.join(' | ')})`);
      // Home carries the most live content (savings advice with clocks, hourly bars) → wider tolerance.
      if (r.diff != null && r.diff > (r.key.startsWith('home') ? DIFF_FAIL * 1.7 : DIFF_FAIL)) fail.push(`${r.key}: ${(r.diff * 100).toFixed(1)}% pixels differ from baseline`);
      if (r.errors.length) fail.push(`${r.key}: JS error ${r.errors[0].slice(0, 120)}`);
    }
    for (const m of motion) if (m.smil || m.running) fail.push(`§10.5 ${m.scenario}: ${m.smil} SMIL + ${m.running} infinite animations under reduced motion`);
    for (const s of stability) if (s.recreated) fail.push(`§10.2 ${s.view}: ${s.recreated}/${s.tracked} schema SVGs recreated in 20 s`);
    if (noGenText.length) fail.push(`§10.3 home without generation mentions: ${noGenText.join(', ')}`);

    const report = { app: APP, at: new Date().toISOString(), widths: WIDTHS, results, motion, stability, noGenText, fail };
    fs.writeFileSync(path.join(OUT, 'report.json'), JSON.stringify(report, null, 2));
    const md = [`# ui-check ${report.at}`, '', `Cases: ${results.length} · failures: ${fail.length}`, '',
      ...fail.map((f) => `- ❌ ${f}`), '', '## §10.5 reduced motion', ...motion.map((m) => `- ${m.scenario}: SMIL ${m.smil}, infinite animations ${m.running}`),
      '', '## §10.2 stability (20 s)', ...stability.map((s) => `- ${s.view}: ${s.recreated}/${s.tracked} recreated`),
      '', `## §10.3 no generation: ${noGenText.length ? noGenText.join(', ') : 'clean'}`,
      '', '## axe (all impacts)', ...results.filter((r) => r.axe.length).map((r) => `- ${r.key}: ${r.axe.map((v) => `${v.id}(${v.impact})×${v.n} \`${(v.html[0] || '').replace(/`/g, "'")}\``).join(', ')}`),
      '', '## clipped text (warning)', ...results.filter((r) => r.clipped.length).map((r) => `- ${r.key}: ${r.clipped.slice(0, 5).join(' | ')}`)];
    fs.writeFileSync(path.join(OUT, 'report.md'), md.join('\n') + '\n');
    console.log(`\n${fail.length ? fail.map((f) => '❌ ' + f).join('\n') : '✅ all checks passed'}\nreport: ${path.join(OUT, 'report.md')}`);
    process.exitCode = fail.length ? 1 : 0;
  } finally {
    await t.close();
  }
}

main().catch((e) => { console.error(e); process.exit(2); });
