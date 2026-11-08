#!/usr/bin/env node
/**
 * espnow-monitor — a READ-ONLY bench monitor for ESP-NOW remote nodes (hub fw 1.1.7+).
 * Used by the scenarios in documents/testing/ESPNOW_NODE_TEST_SCENARIOS.md (link loss / recovery,
 * hub restart, stability soak). It only sends GETs; any change on the hub is done by hand or by
 * the panel while this runs.
 *
 *   node scripts/bench/espnow-monitor.mjs --hub http://<hub> [--interval 2] [--duration 3600]
 *        [--out run.jsonl] [--history] [--max-offline-s 30] [--max-loss-pct 2] [--min-availability 99]
 *        [--max-energy-diff-pct 5] [--period-s 900]
 *
 * --period-s: a node's energy period when the hub doesn't send `period_s` (production 900; a test-build
 *   node may close every 60 s). Periods a hub drops while a node is turned off are expected (the hub
 *   discards a disabled node's data, no backfill) and reported apart, not as a FAIL.
 *
 * A read token (if the hub has one) comes from the env var RBGRID_TOKEN; it is never printed.
 * Ctrl-C ends the run early and still prints the summary. Exit code 1 when a criterion fails.
 *
 * Each poll reads GET /api/v1/espnow and GET /api/telemetry/latest. It prints events as they happen:
 *   node online/offline (with how long it was gone), appeared/removed, disabled/enabled,
 *   hub restart (boot_id), radio on/off, pairing window, periods dropped/duplicated,
 *   a node point's present flag, an energy counter that went backwards, poll failures.
 * The summary gives per-node availability, offline episodes, loss and sync ranges, the realtime
 * frame rate, 15-min periods received vs expected, and PASS/FAIL against the criteria.
 */
import fs from 'node:fs';

const args = Object.fromEntries(process.argv.slice(2).reduce((acc, a, i, all) => {
  if (a.startsWith('--')) acc.push([a.slice(2), all[i + 1] && !all[i + 1].startsWith('--') ? all[i + 1] : 'true']);
  return acc;
}, []));
if (!args.hub || args.hub === 'true') { console.error('usage: espnow-monitor.mjs --hub http://<hub address> [options] (see the header)'); process.exit(2); }
const HUB = args.hub.replace(/\/$/, '');
const INTERVAL_S = Number(args.interval ?? 2);
const DURATION_S = Number(args.duration ?? 0); // 0 = until Ctrl-C
const OUT = args.out;
const CHECK_HISTORY = args.history === 'true';
const MAX_OFFLINE_S = Number(args['max-offline-s'] ?? 30);
const MAX_LOSS_PCT = Number(args['max-loss-pct'] ?? 2);
const MIN_AVAIL = Number(args['min-availability'] ?? 99);
const MAX_ENERGY_DIFF = Number(args['max-energy-diff-pct'] ?? 5);
const PERIOD_S = Number(args['period-s'] ?? 900);
const TOKEN = process.env.RBGRID_TOKEN;
const headers = TOKEN ? { Authorization: `Bearer ${TOKEN}` } : {};

const t0 = Date.now();
const iso = (ms = Date.now()) => new Date(ms).toISOString().slice(11, 19);
const rel = (ms = Date.now()) => `+${((ms - t0) / 1000).toFixed(0)}s`;
const out = OUT ? fs.createWriteStream(OUT, { flags: 'a' }) : null;
const events = [];
function event(kind, text, extra = {}) {
  const e = { t: Date.now(), kind, text, ...extra };
  events.push(e);
  console.log(`${iso(e.t)} ${rel(e.t).padStart(7)}  ${kind.padEnd(10)} ${text}`);
  out?.write(JSON.stringify({ event: e }) + '\n');
}

async function get(path) {
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), 8000);
  try {
    const res = await fetch(HUB + path, { headers, signal: ctl.signal });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    return await res.json();
  } finally { clearTimeout(timer); }
}

// ---- state ------------------------------------------------------------------------------
let polls = 0, failures = 0, failStreak = 0;
let prev = null;          // last espnow answer
let prevTel = null;       // last telemetry frame
let first = null;         // first espnow answer (counter baselines)
let firstAt = 0;
const nodes = new Map();  // mac → { seen, online, offlineSince, episodes: [s], onlinePolls, polls, loss: [], resid: [], skew: [], maxLastSeenOnline }
const units = new Map();  // unit key → { presentPolls, polls, eStart, eLast }
let restarts = 0, dropped = 0, droppedOff = 0, dup = 0;
let lastPollAt = null;
let rtStart = null, periodsStart = null;

function nodeRec(mac) {
  if (!nodes.has(mac)) nodes.set(mac, { online: null, offlineSince: null, episodes: [], onlinePolls: 0, polls: 0, loss: [], resid: [], skew: [], maxLastSeenOnline: 0, upS: 0, periodS: PERIOD_S });
  return nodes.get(mac);
}

function onEspnow(s, tel) {
  const now = Date.now();
  if (!first) { first = s; firstAt = now; rtStart = s.rt_recv; periodsStart = s.periods_recv ?? 0; }
  // hub
  if (prev) {
    if (prev.running !== s.running) event('radio', `running ${prev.running} → ${s.running} (enabled ${s.enabled ?? '?'})`);
    if (prev.pairing_open !== s.pairing_open) event('pairing', s.pairing_open ? `window open, ${s.pairing_left_s ?? '?'} s left` : `window closed, paired ${s.paired_count}`);
    if ((s.periods_dropped ?? 0) > (prev.periods_dropped ?? 0)) {
      const d = s.periods_dropped - prev.periods_dropped;
      const off = [...(s.nodes ?? []), ...(prev.nodes ?? [])].filter((n) => n.disabled).map((n) => n.mac);
      if (off.length) { droppedOff += d; event('periods', `dropped +${d} while ${[...new Set(off)].join(', ')} turned off (expected: no backfill)`); }
      else { dropped += d; event('PERIODS', `dropped +${d} (total ${s.periods_dropped})`); }
    }
    if ((s.periods_dup ?? 0) > (prev.periods_dup ?? 0)) { dup += s.periods_dup - prev.periods_dup; event('PERIODS', `duplicate +${s.periods_dup - prev.periods_dup} (total ${s.periods_dup})`); }
    if (s.rt_recv < prev.rt_recv) { rtStart = s.rt_recv; periodsStart = s.periods_recv ?? 0; }
  }
  // nodes
  const macs = new Set((s.nodes ?? []).map((n) => n.mac));
  for (const mac of nodes.keys()) if (!macs.has(mac) && prev?.nodes?.some((n) => n.mac === mac)) event('node', `${mac} removed from the registry`);
  for (const n of s.nodes ?? []) {
    const r = nodeRec(n.mac);
    const was = prev?.nodes?.find((x) => x.mac === n.mac);
    if (!was && prev) event('node', `${n.mac} appeared (online ${n.online}, channels ${(n.channels ?? []).map((c) => c.ch).join(',')})`);
    if (was && was.disabled !== n.disabled) event('node', `${n.mac} ${n.disabled ? 'disabled' : 'enabled'}`);
    r.polls++;
    if (n.online) r.onlinePolls++;
    if (typeof n.period_s === 'number' && n.period_s > 0) r.periodS = n.period_s;
    // time this node was up and taken in → periods to expect from it
    if (lastPollAt != null && n.online && !n.disabled && was?.online && !was.disabled) r.upS += (now - lastPollAt) / 1000;
    if (r.online !== null && r.online !== n.online) {
      if (n.online) {
        const gone = r.offlineSince ? (now - r.offlineSince) / 1000 : null;
        if (gone != null) r.episodes.push(gone);
        event('ONLINE', `${n.mac} back${gone != null ? ` after ${gone.toFixed(0)} s` : ''} (last_seen ${n.last_seen_s ?? '—'} s)`);
        r.offlineSince = null;
      } else {
        r.offlineSince = now;
        event('OFFLINE', `${n.mac} offline (last_seen ${n.last_seen_s ?? '—'} s)`);
      }
    } else if (r.online === null && !n.online) r.offlineSince = now;
    r.online = n.online;
    if (n.online && typeof n.last_seen_s === 'number') r.maxLastSeenOnline = Math.max(r.maxLastSeenOnline, n.last_seen_s);
    // loss_pct is a ≥ 60 s window (fw e163afd), absent until the first window completes
    if (n.sync) { if (typeof n.sync.loss_pct === 'number') r.loss.push(n.sync.loss_pct); r.resid.push(n.sync.resid_sd_us); r.skew.push(n.sync.skew_ppm); }
  }
  // node points in telemetry
  if (tel?.units) {
    if (prevTel && prevTel.boot_id !== tel.boot_id) { restarts++; event('RESTART', `hub restarted (boot_id ${prevTel.boot_id} → ${tel.boot_id})`); }
    for (const [key, u] of Object.entries(tel.units)) {
      if (u.src !== 'espnow') continue;
      if (!units.has(key)) units.set(key, { presentPolls: 0, polls: 0, eStart: u.e_in ?? 0, eLast: u.e_in ?? 0, point: u.point, intWh: 0, lastT: null, lastP: null });
      const r = units.get(key);
      const pu = prevTel?.units?.[key];
      r.polls++;
      if (u.present) r.presentPolls++;
      if (pu && pu.present !== u.present) event('point', `${key} (${u.point ?? ''}) present ${pu.present} → ${u.present}`);
      if (pu && typeof u.e_in === 'number' && typeof pu.e_in === 'number' && u.e_in + 1e-9 < pu.e_in && prevTel.boot_id === tel.boot_id) {
        event('ENERGY', `${key} e_in went backwards ${pu.e_in} → ${u.e_in}`);
      }
      if (typeof u.e_in === 'number') r.eLast = u.e_in;
      // Energy from the realtime power (trapezoid over polls while present), to compare with e_in.
      const now = Date.now();
      const p = u.present && typeof u.p_act === 'number' ? Math.max(0, u.p_act) : null;
      if (p != null && r.lastP != null && r.lastT != null) r.intWh += ((p + r.lastP) / 2) * ((now - r.lastT) / 3600000);
      r.lastT = now; r.lastP = p;
    }
  }
  prev = s;
  prevTel = tel ?? prevTel;
  lastPollAt = now;
}

async function poll() {
  polls++;
  try {
    const [s, tel] = await Promise.all([get('/api/v1/espnow'), get('/api/telemetry/latest').catch(() => null)]);
    if (failStreak > 0) event('hub', `answers again after ${failStreak} failed poll(s)`);
    failStreak = 0;
    out?.write(JSON.stringify({ t: Date.now(), espnow: s, units: tel ? Object.fromEntries(Object.entries(tel.units ?? {}).filter(([, u]) => u.src === 'espnow')) : null, boot_id: tel?.boot_id }) + '\n');
    onEspnow(s, tel);
  } catch (e) {
    failures++; failStreak++;
    if (failStreak === 1) event('hub', `no answer (${e.message ?? e})`);
  }
}

async function historyGaps() {
  const res = [];
  for (const key of units.keys()) {
    try {
      const h = await get(`/api/v1/history?unit_id=${key}&period=hour&type=energy&limit=48`);
      const ts = (h.data ?? []).map((r) => r.ts).filter((x) => typeof x === 'number').sort((a, b) => a - b);
      const gaps = [];
      for (let i = 1; i < ts.length; i++) if (ts[i] - ts[i - 1] > 3600 + 120) gaps.push([ts[i - 1], ts[i]]);
      res.push({ key, records: ts.length, gaps });
    } catch (e) { res.push({ key, error: String(e.message ?? e) }); }
  }
  return res;
}

const stat = (xs) => xs.length ? { min: Math.min(...xs), max: Math.max(...xs), mean: xs.reduce((a, b) => a + b, 0) / xs.length } : null;
const f1 = (x) => (x == null ? '—' : (Math.round(x * 10) / 10).toString());

async function summary() {
  const el = (Date.now() - t0) / 1000;
  const checks = [];
  const check = (ok, text) => { checks.push(ok); console.log(`  ${ok ? 'PASS' : 'FAIL'}  ${text}`); };
  console.log(`\n=== summary: ${f1(el)} s, ${polls} polls, ${failures} failed ===`);
  if (prev && first) {
    const span = (Date.now() - firstAt) / 1000;
    const rt = (prev.rt_recv - rtStart) / Math.max(1, span);
    const per = (prev.periods_recv ?? 0) - periodsStart;
    const expected = [...nodes.values()].reduce((a, r) => a + r.upS / r.periodS, 0);
    console.log(`hub: radio ${prev.running ? 'on' : 'off'} (${prev.enabled ?? '?'}), channel ${prev.channel}, keys ${prev.keys}, restarts ${restarts}`);
    console.log(`     realtime frames ${f1(rt)}/s (≈1/s per online node), periods received ${per} (expect ≈ ${f1(expected)} from the nodes' up time), dropped ${dropped}${droppedOff ? ` (+${droppedOff} while a node was turned off: expected)` : ''}, duplicate ${dup}`);
    check(dropped === 0, `no energy period dropped (outside turned-off spans)`);
    check(dup === 0, `no energy period duplicated`);
    if (expected >= 2) check(Math.abs(per - expected) <= Math.max(1.5, expected * 0.05), `periods received ≈ expected (± 1, or 5 %)`);
  }
  for (const [mac, r] of nodes) {
    const avail = r.polls ? (100 * r.onlinePolls) / r.polls : 0;
    const ep = stat(r.episodes), loss = stat(r.loss), resid = stat(r.resid), skew = stat(r.skew);
    console.log(`node ${mac}: online ${f1(avail)} % of polls, offline episodes ${r.episodes.length}${ep ? ` (max ${f1(ep.max)} s, mean ${f1(ep.mean)} s)` : ''}${r.offlineSince ? `, OFFLINE now for ${f1((Date.now() - r.offlineSince) / 1000)} s` : ''}`);
    console.log(`     loss % ${loss ? `${f1(loss.min)}–${f1(loss.max)} (mean ${f1(loss.mean)})` : '— (no sync: not heard)'}, sync resid µs ${resid ? `${f1(resid.min)}–${f1(resid.max)}` : '—'}, skew ppm ${skew ? `${f1(skew.min)}–${f1(skew.max)}` : '—'}, max last_seen while online ${r.maxLastSeenOnline} s`);
    check(avail >= MIN_AVAIL, `${mac} online ≥ ${MIN_AVAIL} % of polls`);
    const ongoing = r.offlineSince ? (Date.now() - r.offlineSince) / 1000 : 0;
    check(Math.max(ep?.max ?? 0, ongoing) <= MAX_OFFLINE_S, `${mac} no offline episode longer than ${MAX_OFFLINE_S} s (the one still open counts)`);
    if (loss) check(loss.mean <= MAX_LOSS_PCT, `${mac} mean loss ≤ ${MAX_LOSS_PCT} %`);
  }
  for (const [key, r] of units) {
    const counted = r.eLast - r.eStart; // e_in is Wh on the wire
    console.log(`point ${key} (${r.point ?? ''}): present ${f1(r.polls ? (100 * r.presentPolls) / r.polls : 0)} % of polls, energy in +${f1(counted)} Wh by the counter, ${f1(r.intWh)} Wh from realtime power`);
    // Only meaningful with a steady load and a short poll interval; skipped under 5 Wh.
    if (counted >= 5) check(Math.abs(r.intWh - counted) / counted <= MAX_ENERGY_DIFF / 100, `${key} counter vs realtime power within ${MAX_ENERGY_DIFF} %`);
  }
  check(!events.some((e) => e.kind === 'ENERGY'), 'no energy counter went backwards');
  if (CHECK_HISTORY) {
    for (const h of await historyGaps()) {
      if (h.error) console.log(`history ${h.key}: ${h.error}`);
      else { console.log(`history ${h.key}: ${h.records} hour records, gaps ${h.gaps.length}`); check(h.gaps.length === 0, `${h.key} no gap between hour records`); }
    }
  }
  const ok = checks.every(Boolean);
  console.log(ok ? '\nRESULT: PASS' : '\nRESULT: FAIL');
  out?.end();
  process.exit(ok ? 0 : 1);
}

let stopping = false;
process.on('SIGINT', () => { if (!stopping) { stopping = true; void summary(); } });
console.log(`espnow-monitor → ${HUB}, every ${INTERVAL_S} s${DURATION_S ? ` for ${DURATION_S} s` : ' until Ctrl-C'}${TOKEN ? ' (read token from env)' : ''}`);
(async () => {
  while (!stopping && (!DURATION_S || (Date.now() - t0) / 1000 < DURATION_S)) {
    const started = Date.now();
    await poll();
    const wait = INTERVAL_S * 1000 - (Date.now() - started);
    if (wait > 0) await new Promise((r) => setTimeout(r, wait));
  }
  if (!stopping) { stopping = true; await summary(); }
})();
