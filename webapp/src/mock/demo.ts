/**
 * Demo data — synthetic telemetry/sensors/history so the app is fully viewable WITHOUT a
 * device (gated on ENVIRONMENT.USE_MOCK_DATA, i.e. `npm start` dev mode). Production talks
 * to the real device. This feeds the canonical shapes (TelemetryFrame / SensorListEntry /
 * History records), so screens render exactly as they would against firmware.
 */
import { TelemetryFrame, TelemetryUnit } from '../types/telemetry-units';
import { SensorListEntry, SensorModule, CreateUnitRequest, UnitCreateResult, UnitDeleteResult, ModuleFwProgress, ModuleFwStart } from '../types/sensors';
import { HistoryRecord, HistoryType, HistoryPeriod } from '../types/history';
import { SystemConfig, TimeConfigView, TelemetryConfigView } from '../types/config';
import { WifiStatus } from '../types/network';
import { TariffPlan, TariffPlanConfigData } from '../types/tariff-plan';
import { BaseResponse } from '../types/api';
import { HubEvent, HubEventsPage, HubVersion } from '../types/hubEvents';
import { demoVariant } from './demoVariant';
import { demoSite, demoPoint, DemoRole } from './demoSites';

export { DEMO_CT_MODELS } from './demoSites';

// The public demo is English whatever the interface language (operator 20.10 — «Первый этаж»
// showed in the English demo): site / point / group / plan names in mock/demoSites are English.

/** Lifetime counters (Wh) at today's midnight — the live e_in/e_out and the history's et_in/et_out
 *  are both counted from them, so the running hour (live − last record) is a real hour. */
const E_IN_BASE = 125_000;
const E_OUT_BASE = 1_850_000;

export function demoEnvelope<T>(type: string, data: T): BaseResponse<T> {
  const now = Date.now();
  return {
    type, version: '2.0', device_id: 'rbgrid_demo_001',
    timestamp_local: Math.floor(now / 1000), timestamp_ntp: Math.floor(now / 1000), data,
  };
}

const jitter = (base: number, pct = 0.04) => base * (1 + (Math.random() - 0.5) * pct);

let demoSeq = 1000; // monotonic frame counter (so the "data delayed" seq-gap logic has input)
const DEMO_BOOT_ID = 2896034471;

interface DemoUnitOpts {
  point?: string;
  src: 'i2c';
  srcClass: 'rbamp';
  caps: string[];
  dayIn?: number;
  dayOut?: number;
  bus?: number; module?: number; ch?: number;
}

/**
 * A synthetic per-unit telemetry object. rbamp units omit pf/f (fields_by_source) so the
 * UI's dash-not-0.00 gating is visible.
 */
function demoUnit(p: number, o: DemoUnitOpts): TelemetryUnit {
  // One CT = one conductor (root delta №4): a two-pole 240 V line reads its leg's 120 V.
  const v = jitter(demoRegion().v);
  const i = jitter(Math.abs(p) / v);
  return {
    present: true, src: o.src, src_class: o.srcClass, point: o.point, caps: o.caps,
    bus: o.bus ?? 0, module: o.module, ch: o.ch,
    // Current firmware (root f451b6a5): the SIGN of p_act is the direction (+ import, − export);
    // no DIRECTION cap / dir field on rbAmp units.
    v, i, p_act: jitter(p),
    // A generation line takes nothing; its lifetime output is a few MWh, not «2,4 kWh» (GEN-R05).
    e_in: o.dayOut && !o.dayIn ? 0 : E_IN_BASE + (o.dayIn ?? 3200), e_day_in: o.dayIn ?? 3200,
    e_out: o.dayOut ? E_OUT_BASE + o.dayOut : 0, e_day_out: o.dayOut ?? 0,
    energy_mode: 'on_device', q_live: 0, res: 1,
    result: { v, i, p_act: jitter(p) },
  };
}

/**
 * One synthetic telemetry frame (values jitter each call to look live). Shaped like the real
 * hub on canon fc0f962b (verified on .139): supplies keyed by the LEGACY keys "main"/"v_bus"
 * (schema key_format.legacy_supply_keys), lines by HEX4, `dir` as the firmware enum, energy
 * in Wh, money in major units, live tariff in system.tariff (E1).
 */
/**
 * Demo region «US split-phase» (spec П2 — a multi-phase site): localStorage
 * `rbgrid.demo.region = "us"` → 120/240 V split-phase service, 60 Hz, USD (system.grid
 * declared, two-pole 240 V loads on phase L1L2). Default = EU single-input 230 V / 50 Hz, RUB.
 */
export interface DemoRegion { us: boolean; v: number; hz: number; cur: string; k: number }
export function demoRegion(): DemoRegion {
  const us = demoVariant().us;
  // k converts the RUB demo money into the region's currency (6,43 ₽ → $0.18 · €0.30 per kWh).
  // Europe: roubles for a Russian interface, euros otherwise (the public demo is English).
  if (us) return { us, v: 120, hz: 60, cur: 'USD', k: 0.028 };
  return { us, v: 230, hz: 50, cur: 'EUR', k: 0.047 };
}
/** Demo money in the region's currency (2 decimals). */
export const dm = (rub: number): number => Math.round(rub * demoRegion().k * 100) / 100;

/** The active site's two-zone plan, rates NET per kWh (the hub's wire; the UI adds VAT). */
export interface DemoPlan { plan: string; provider: string; dayFrom: number; dayTo: number; day: number; night: number; vat: number; fixed: number; sell: number | null }
export function demoTariff(): DemoPlan {
  const t = demoSite().tariff;
  if (!t) return { plan: 'Two-rate (demo)', provider: 'Utility (demo)', dayFrom: 7, dayTo: 23, day: dm(6.43), night: dm(3.21), vat: 20, fixed: dm(120), sell: dm(1.8) };
  const net = (g: number) => Math.round((g / (1 + t.vat / 100)) * 10_000) / 10_000;
  return { plan: t.plan, provider: t.provider, dayFrom: t.dayFrom, dayTo: t.dayTo, day: net(t.dayGross), night: net(t.nightGross), vat: t.vat, fixed: t.fixed, sell: t.sell };
}
/** Local hour `h` is in the plan's Day zone. */
export const demoIsDay = (h: number): boolean => { const t = demoTariff(); return h >= t.dayFrom && h < t.dayTo; };

// ---- Demo point lifecycle (UX audit W0-2/W0-9): delete / bind act on the demo like on a hub,
// kept in this browser only, so «Удалить точку» / «Отвязать» / «Привязать» are honest in demo. ----
const DEMO_UNITS_KEY = 'rbgrid.demo.units';
interface DemoAddedUnit { unit_id: string; name: string; role?: string; module: number; ch: number }
interface DemoUnitsState { removed: string[]; added: DemoAddedUnit[] }
const DEMO_LOCKED = new Set(['9001']); // the input — like the hub, not deletable from the list

function demoUnitsState(): DemoUnitsState {
  try {
    const s = JSON.parse(localStorage.getItem(DEMO_UNITS_KEY) ?? '{}') as Partial<DemoUnitsState>;
    return { removed: s.removed ?? [], added: s.added ?? [] };
  } catch { return { removed: [], added: [] }; }
}
function saveDemoUnits(s: DemoUnitsState): void {
  try { localStorage.setItem(DEMO_UNITS_KEY, JSON.stringify(s)); } catch { /* storage off → no-op */ }
}

export function demoDeleteUnit(hexId: string): UnitDeleteResult {
  const id = hexId.toUpperCase();
  if (DEMO_LOCKED.has(id)) return { status: 'error', message: 'main supply unit cannot be removed' };
  const s = demoUnitsState();
  if (s.added.some((u) => u.unit_id === id)) s.added = s.added.filter((u) => u.unit_id !== id);
  else if (!s.removed.includes(id)) s.removed.push(id);
  saveDemoUnits(s);
  return { status: 'ok', removed: id, persist: 'ESP_OK' };
}

export function demoCreateUnit(body: CreateUnitRequest): UnitCreateResult {
  const { module, ch } = body.sensor as { module?: number; ch?: number };
  const sensor = demoSensors().find((x) => x.module === module && x.ch === ch);
  if (!sensor) return { status: 'error', message: 'sensor not found' };
  if (sensor.unit_id) return { status: 'error', message: 'sensor already bound' };
  const s = demoUnitsState();
  const base = demoSensorsBase().find((x) => x.module === module && x.ch === ch)?.unit_id;
  let id: string;
  if (base && s.removed.includes(base)) { s.removed = s.removed.filter((x) => x !== base); id = base; }
  else {
    id = `90${module ?? 0}${ch ?? 0}`.slice(0, 4).toUpperCase();
    s.added.push({ unit_id: id, name: body.name, role: body.role, module: module ?? 0, ch: ch ?? 0 });
  }
  saveDemoUnits(s);
  return { status: 'ok', unit_id: id, persist: 'ESP_OK' };
}

/** Apply the demo lifecycle to a units-config document (removed dropped, added appended). */
export function demoAdjustUnitsConfig(doc: Record<string, unknown>): Record<string, unknown> {
  const s = demoUnitsState();
  if (!s.removed.length && !s.added.length) return doc;
  const list = Array.isArray(doc.units) ? (doc.units as Record<string, unknown>[]) : [];
  const hex = (u: Record<string, unknown>) => (typeof u.unit_id === 'number' ? u.unit_id.toString(16) : String(u.unit_id)).toUpperCase().padStart(4, '0');
  const units = [
    ...list.filter((u) => !s.removed.includes(hex(u))),
    ...s.added.filter((a) => !list.some((u) => hex(u) === a.unit_id)).map((a) => ({
      unit_id: parseInt(a.unit_id, 16), point: '', name: a.name, short_name: a.name.slice(0, 12),
      type: 'CURRENT_LOAD', role: a.role ?? 'DEVICE_LOAD', phase: 0, rated_voltage_v: demoRegion().v,
    })),
  ];
  return { ...doc, units };
}

export function demoTelemetryFrame(): TelemetryFrame {
  const reg = demoRegion();
  const site = demoSite();
  const inp = demoToday('input');
  const units: Record<string, TelemetryUnit> = {};
  for (const pt of site.points) {
    const day = demoToday(pt.profile);
    const gen = pt.role === 'RENEWABLE_SOURCE';
    // US: a two-pole 240 V line with ONE CT reads one conductor — half of the full power (W9 ×2).
    const p = day.nowW * (pt.pole ?? 1);
    const u = demoUnit(p, {
      point: pt.point || undefined, src: 'i2c', srcClass: 'rbamp', caps: ['DEVICE_ENERGY', 'PERSISTED'],
      module: pt.module, ch: pt.ch, dayIn: gen ? 0 : Math.round(day.inWh), dayOut: gen ? Math.round(day.outWh) : undefined,
    });
    if (pt.absent) { units[pt.id] = { ...u, present: false, p_act: 0, i: 0 }; continue; }
    // The CT's load (BASIC d03f19ed): % of its rating, near the limit ≥ 85 %, clipping ≥ 100 %.
    const loadPct = Math.round(((u.i ?? 0) / pt.ct.a) * 100);
    const ctState = pt.ch === 254 ? {} : { load_pct: loadPct, ...(loadPct >= 100 ? { clip: true } : loadPct >= 85 ? { ct_warn: true } : {}) };
    // A three-phase input (STD-U3I3 group): three legs, slightly unbalanced; US: two legs. A site
    // with every line on a phase sums each leg from its points (+ the unmetered rest split evenly).
    const legW = site.legsFromPoints && pt.ch === 254
      ? [1, 2, 3].map((n) => site.points.filter((x) => x.phase === n && !x.absent).reduce((sum, x) => sum + demoToday(x.profile).nowW, 0) + site.restW(new Date().getHours(), true) / 3)
      : null;
    const ph = pt.ch !== 254 ? undefined : legW
      ? legW.map((w, n) => {
        const v = jitter([231.4, 229.8, 230.6][n], 0.003);
        return { ph: n + 1, v, i: jitter(w / v), p: jitter(w), q: jitter(w * 0.18), pf: [0.95, 0.96, 0.97][n] };
      })
      : reg.us
      ? [
        { ph: 1, v: jitter(121.2, 0.003), i: jitter(Math.abs(p * 0.55) / 121.2), p: jitter(p * 0.55), q: jitter(Math.abs(p * 0.55) * 0.09), pf: 0.97 },
        { ph: 2, v: jitter(119.6, 0.003), i: jitter(Math.abs(p * 0.45) / 119.6), p: jitter(p * 0.45) },
      ]
      : [0.36, 0.33, 0.31].map((k, n) => {
        const v = jitter([231.4, 229.8, 230.6][n], 0.003);
        return { ph: n + 1, v, i: jitter((p * k) / v), p: jitter(p * k), q: jitter(p * k * 0.22), pf: [0.93, 0.92, 0.94][n] };
      });
    units[pt.id] = { ...u, ...ctState, ...(ph ? { ph } : {}) } as TelemetryUnit;
  }
  const st = demoUnitsState();
  for (const id of st.removed) delete units[id];
  for (const a of st.added) {
    units[a.unit_id] = demoUnit(150, { src: 'i2c', srcClass: 'rbamp', caps: ['DEVICE_ENERGY', 'PERSISTED'], module: a.module, ch: a.ch, dayIn: 400 });
  }
  // The input carries every line → net = the input (the site minus the sun).
  const net = inp.nowW;
  // The site plan's zone by the clock — the «сейчас» badge and the tariff advice follow real time
  // instead of a pinned «День».
  const plan = demoTariff();
  const dayZone = demoIsDay(new Date().getHours());
  const buyRate = dayZone ? plan.day : plan.night;
  return {
    v: 1, seq: demoSeq++, boot_id: DEMO_BOOT_ID, uptime_s: 86645,
    ts: Math.floor(Date.now() / 1000), sntp: Math.floor(Date.now() / 1000),
    tq: 1, time_src: 'sntp', sync_age_s: 312,
    freq: jitter(reg.hz, 0.002), net_dir: net < 0 ? -1 : 1, net_pwr: jitter(Math.abs(net)),
    alarm_count: 0, alarm_max_sev: 0,
    system: {
      p_act: jitter(net), p_rea: jitter(net * 0.08),
      // Today from the hourly model — the same day the History and Economics show.
      e_in: 156_200 + Math.round(inp.inWh), e_day_in: Math.round(inp.inWh), e_out: 2400 + Math.round(inp.outWh), e_day_out: Math.round(inp.outWh),
      cost_total: dm(9_861.4) + inp.cost, cost_day: Math.round(inp.cost * 100) / 100, rev_total: dm(418.2) + inp.rev, rev_day: Math.round(inp.rev * 100) / 100,
      units: Object.keys(units).length, present: Object.values(units).filter((u) => u.present !== false).length,
      tariff: { zone: dayZone ? 'DAY' : 'NIGHT', buy_rate: buyRate, sell_rate: plan.sell ?? 0, cost_rate_h: (Math.max(0, net) / 1000) * buyRate, currency: reg.cur },
    },
    units,
  };
}

/** Sensor registry: bound units + one unbound (re-bindable) sensor; src_class per contract v4.
 *  The demo lifecycle (delete / bind) is applied on top of the canned registry. */
export function demoSensors(): SensorListEntry[] {
  const s = demoUnitsState();
  return demoSensorsBase().map((x) => {
    if (x.unit_id && s.removed.includes(x.unit_id)) return { ...x, unit_id: null };
    const added = s.added.find((a) => a.module === x.module && a.ch === x.ch);
    return added && !x.unit_id ? { ...x, unit_id: added.unit_id } : x;
  });
}

function demoSensorsBase(): SensorListEntry[] {
  // The variant's wiring (mock/demoSites): a point per channel, a phase group as ONE entry ch 254
  // (delta №5 — its channels aren't listed), and the free channels.
  const site = demoSite();
  const addr = (m: number) => site.modules.find((x) => x.module === m)?.addr ?? 0x50 + m;
  return [
    ...site.points.map((p): SensorListEntry => ({ iface: 'i2c', present: !p.absent, unit_id: p.id, bus: 0, module: p.module, ch: p.ch, ct_code: p.ct.code, addr: addr(p.module), src_class: 'rbamp' } as SensorListEntry)),
    ...site.free.map((f): SensorListEntry => ({ iface: 'i2c', present: true, unit_id: null, bus: 0, module: f.module, ch: f.ch, ct_code: 3, addr: addr(f.module), src_class: 'rbamp' } as SensorListEntry)),
  ];
}

/** Demo module update (root 534ded9e): ~2 min on a hub, 24 s here — writing, commit, activation. */
const demoFw = { fw: 65657, bank: '0x08030000', started: 0, before: 0 };
export function demoModuleFwStart(module: number, bank: string | undefined, size: number): ModuleFwStart {
  if (module !== 0 || demoSite().modules[0]?.line !== 'std') return { status: 'error', reason: 'NOT_UPDATABLE', message: 'module has no OTA capability', http: 400 };
  if (demoFw.started && Date.now() - demoFw.started < 24_000) return { status: 'error', reason: 'BUSY', message: 'update in progress', http: 409 };
  if (size !== 180224) return { status: 'error', reason: 'SIZE', message: `image size ${size} != bank size 180224`, http: 400 };
  if (bank === demoFw.bank) return { status: 'error', reason: 'ACTIVE_BANK', message: 'bank is the active one', http: 400 };
  demoFw.started = Date.now(); demoFw.before = demoFw.fw;
  return { status: 'started' };
}
export function demoModuleFw(module: number): ModuleFwProgress {
  if (!demoFw.started || module !== 0) return { module, state: 'idle' };
  const s = (Date.now() - demoFw.started) / 1000, total = 704;
  const base = { module, total, elapsed_s: Math.round(s * 5), fw_before: demoFw.before };
  if (s < 16) return { ...base, state: 'writing', done: Math.round((s / 16) * total) };
  if (s < 18) return { ...base, state: 'committing', done: total };
  if (s < 24) return { ...base, state: 'activating', done: total };
  if (demoFw.fw === demoFw.before) { demoFw.fw = 65659; demoFw.bank = demoFw.bank === '0x08030000' ? '0x08004000' : '0x08030000'; }
  return { ...base, state: 'done', done: total, fw_after: demoFw.fw };
}

/** The variant's rbAmp modules (mock/demoSites); an STD module carries the OTA block. */
export function demoSensorModules(): SensorModule[] {
  return demoSite().modules.map((m) => (m.line === 'std' && m.module === 0
    ? { ...m, fw: demoFw.fw, ota: { bank_size: 180224, active_bank: demoFw.bank, inactive_bank: demoFw.bank === '0x08030000' ? '0x08004000' : '0x08030000', banks: ['0x08004000', '0x08030000'], board: '0x42440863' } }
    : m));
}

/**
 * Synthetic history series (newest-first). Exercises the v4 quality/time states the design
 * pass needs to render: a PARTIAL bucket (q bit0, dur < nominal → hatching + "N of 60 min"),
 * a legacy uptime-ts record (ts < 1.6e9 → "before time sync" group), plus v4 header
 * (dur/q/res), energy totalizers (et_in/et_out) and economics plan tagging.
 */
/** Demo /api/v1/export.csv: the hub's column layout, rows from the same series the charts draw
 *  (UX audit RES-09 — the demo used to hand out a fixed 2024 stub). */
export function demoExportCsv(type: string, period: string): string {
  const recs = demoHistory((type as HistoryType) || 'energy', (period as HistoryPeriod) || 'hour')
    .filter((r) => r.ts > 1.6e9) as unknown as Record<string, number>[];
  const f = (v: number | undefined, d: number) => (typeof v === 'number' ? v.toFixed(d) : '');
  if (type === 'economics') {
    return ['timestamp_unix,cost,revenue,net_cost', ...recs.map((r) => `${r.ts},${f(r.c, 4)},${f(r.r, 4)},${f(r.c - r.r, 4)}`)].join('\n') + '\n';
  }
  if (type === 'electrical') {
    return ['timestamp_unix,v_rms_V,i_rms_A,p_active_W,p_reactive_var,p_apparent_VA,pf,freq_Hz,thd_v_pct,thd_i_pct',
      ...recs.map((r) => `${r.ts},${f(r.v, 2)},${f(r.i, 3)},${f(r.p_act, 1)},${f(r.p_rea, 1)},${f(r.p_app, 1)},${f(r.pf, 2)},${f(r.f, 1)},${f(r.thd_v, 2)},${f(r.thd_i, 2)}`)].join('\n') + '\n';
  }
  return ['timestamp_unix,energy_consumed_Wh,energy_supplied_Wh,energy_net_Wh',
    ...recs.map((r) => `${r.ts},${f(r.ec, 1)},${f(r.es, 1)},${f(r.ec - r.es, 1)}`)].join('\n') + '\n';
}

/**
 * Demo history BY ROLE (UX audit W3-5): each point has its own day — the kitchen peaks morning and
 * evening, ЗС-1 charges in the evening, the solar line produces 06–20 with a different sky every
 * day, and the input is the house minus the sun (exporting at noon). Money by the demo two-zone
 * plan (07–23 day). Buckets end on real hour / midnight / month boundaries (wire `ts` = the END
 * of the interval, canon ed876014), so «сегодня», «вчера» and «этот месяц» line up.
 */
/** A point's day profile — by the variant's site table (mock/demoSites). */
function demoRoleOf(unitId?: string | null): DemoRole {
  return demoPoint(unitId)?.profile ?? 'line';
}

/** The number of the viewer's LOCAL calendar day holding `t` (s) — one rule for the live day,
 *  the history and the counters (the sky, the laundry / charger days change at local midnight). */
const demoDayNo = (t: number): number => Math.floor((t * 1000 - new Date(t * 1000).getTimezoneOffset() * 60000) / 86_400_000);
/** Deterministic sky of a day (0.35 overcast … 1 clear). */
const demoSky = (dayNo: number): number => { const x = Math.sin(dayNo * 12.9898) * 43758.5453; return 0.35 + 0.65 * (x - Math.floor(x)); };
const bump = (h: number, from: number, to: number, w: number): number => (h >= from && h < to ? w : 0);
/**
 * The house's car (heat pump + EV site): 11 kW on arrival, 18:00–20:30 — the Day zone, the
 * «resident's mistake» the app makes visible; three days ago it charged at night instead
 * (22:00–00:30) for contrast. Share of the hour charging.
 */
function demoEvCharge(h: number, dayNo: number): number {
  const contrast = demoDayNo(Date.now() / 1000) - 3;
  if (dayNo === contrast) return h >= 22 ? 1 : 0;
  if (dayNo === contrast + 1 && h === 0) return 0.5;
  return h === 18 || h === 19 ? 1 : h === 20 ? 0.5 : 0;
}
/** Average W of a role in local hour `h` of day `dayNo` (negative = generation). */
function demoHourW(role: DemoRole, h: number, dayNo: number): number {
  // A working day (Mon–Fri) for the workshop; weekends quiet.
  const workday = new Date(dayNo * 86_400_000 + 43_200_000).getUTCDay() % 6 !== 0;
  const wk = (base: number, from: number, to: number, add: number) => base + (workday ? bump(h, from, to, add) : 0);
  switch (role) {
    case 'kitchen': return 40 + bump(h, 7, 9, 400) + bump(h, 12, 14, 250) + bump(h, 18, 21, 550);
    case 'ev1': return bump(h, 19, 24, 3300) + bump(h, 0, 1, 3300);
    // A home charger on the night tariff (operator 20.10: the 3.3 kW evening-to-1 am block read as a
    // spike at midnight): 05–07 every morning, 2.3 kW — still the night zone (23–07), and outside
    // the 01–05 window the standby estimate reads (charging there made «standby 1.5 kW»).
    case 'homeEv': return bump(h, 5, 7, 2300);
    // Car park stations: sessions spread over the day, so a visitor at any hour sees one charging.
    case 'ev2': return bump(h, 7, 10, 3600) + bump(h, 12, 15, 7000) + (dayNo % 2 === 0 ? bump(h, 16, 18, 2100) : 0);
    case 'ev3': return bump(h, 9, 12, 7000) + bump(h, 15, 19, 6500) + bump(h, 2, 5, 3700);
    case 'solar': return -2600 * demoSky(dayNo) * Math.max(0, Math.sin((Math.PI * (h + 0.5 - 6)) / 14)) ** 1.5;
    case 'garage': return 90 + bump(h, 17, 20, 300);
    case 'off': return 0;
    case 'line': return 60 + bump(h, 18, 22, 90);
    // the flat
    case 'heater': return 15 + bump(h, 6, 8, 2000) + bump(h, 20, 22, 2000);
    case 'laundry': return 5 + (dayNo % 2 === 0 ? bump(h, 10, 12, 1900) : bump(h, 19, 21, 1700));
    case 'living': return 70 + bump(h, 18, 23, 320);
    case 'bedrooms': return 25 + bump(h, 21, 24, 140) + bump(h, 6, 8, 90);
    // the workshop
    case 'workshop': return wk(250, 8, 18, 5200 + (h === 12 ? -2600 : 0));
    case 'lighting': return wk(90, 7, 19, 1100);
    case 'compressor': return wk(0, 8, 18, h % 2 === 0 ? 3800 : 1500);
    case 'hvac': return 350 + wk(0, 9, 18, 2600 + (h >= 13 && h < 16 ? 900 : 0));
    case 'office': return wk(120, 8, 18, 650);
    // the car park
    case 'building': return 300 + bump(h, 8, 20, 900);
    case 'parkLight': return h >= 18 || h < 7 ? 1100 : 60;
    // the house with a heat pump and an EV (site brief 02.11), W per conductor
    case 'hpEv': return 3680 * demoEvCharge(h, dayNo);
    // Cycles of ~2.25 kW (750 W a phase); the hour's mean = the share of the hour it runs.
    case 'heatPump': return 750 * (h < 6 ? 0.5 : h < 9 ? 0.6 : h < 17 ? 0.25 : h < 22 ? 0.4 : 0.5);
    case 'hpKitchen': return 30 + bump(h, 7, 8, 1200) + bump(h, 17, 18, 900) + bump(h, 18, 19, 400) + bump(h, 19, 20, 1200) + bump(h, 20, 22, 200);
    case 'input':
    default: {
      // The input carries the site: every present line (generation negative) + the unmetered rest.
      const site = demoSite();
      const lines = site.points.filter((p) => p.profile !== 'input' && !p.absent)
        .reduce((sum, p) => sum + demoHourW(p.profile, h, dayNo), 0);
      return lines + site.restW(h, workday);
    }
  }
}

const demoRateAt = (h: number): number => (demoIsDay(h) ? demoTariff().day : demoTariff().night);
const demoSellRate = (): number => demoTariff().sell ?? 0;

/** Today so far for a role, from the same hourly model as the history (the running hour pro
 *  rata) — the live day counters and the power now agree with the charts (W3-5). */
export function demoToday(role: DemoRole): { inWh: number; outWh: number; cost: number; rev: number; nowW: number } {
  const now = new Date();
  const mid = new Date(now); mid.setHours(0, 0, 0, 0);
  const dayNo = demoDayNo(mid.getTime() / 1000);
  let inWh = 0, outWh = 0, cost = 0, rev = 0;
  for (let h = 0; h <= now.getHours(); h++) {
    const frac = h < now.getHours() ? 1 : now.getMinutes() / 60;
    const w = demoHourW(role, h, dayNo);
    const imp = Math.max(0, w) * frac, exp = Math.max(0, -w) * frac;
    inWh += imp; outWh += exp; cost += (imp / 1000) * demoRateAt(h); rev += (exp / 1000) * demoSellRate();
  }
  return { inWh, outWh, cost, rev, nowW: demoHourW(role, now.getHours(), dayNo) };
}

/** The input's money by zone over the hub's uptime (~24 h, the frame's uptime_s) — the
 *  Economics «by zone since the last restart» from the same hourly model as the day. */
export function demoZoneMoney(): { day: { cost: number; rev: number }; night: { cost: number; rev: number } } {
  const out = { day: { cost: 0, rev: 0 }, night: { cost: 0, rev: 0 } };
  const nowS = Date.now() / 1000;
  for (let t0 = nowS - 86_400; t0 < nowS; t0 += 3600) {
    const d = new Date(t0 * 1000);
    const w = demoHourW('input', d.getHours(), demoDayNo(t0));
    const z = demoIsDay(d.getHours()) ? out.day : out.night;
    z.cost += (Math.max(0, w) / 1000) * demoRateAt(d.getHours());
    z.rev += (Math.max(0, -w) / 1000) * demoSellRate();
  }
  const r2 = (v: number) => Math.round(v * 100) / 100;
  return { day: { cost: r2(out.day.cost), rev: r2(out.day.rev) }, night: { cost: r2(out.night.cost), rev: r2(out.night.rev) } };
}

export function demoHistory(type: HistoryType, period: HistoryPeriod, n = 48, unitId?: string | null): HistoryRecord[] {
  const role = demoRoleOf(unitId);
  const nowMs = Date.now();
  const at = (d: Date) => Math.floor(d.getTime() / 1000);
  // Bucket ENDS (newest first): closed buckets only — the running one is not a record yet.
  const ends: number[] = [];
  const count = period === 'year' ? 3 : period === 'month' ? 14 : period === 'week' ? 10 : n;
  for (let k = 0; k < count; k++) {
    const d = new Date(nowMs);
    if (period === 'hour') { d.setMinutes(0, 0, 0); d.setHours(d.getHours() - k); }
    else if (period === 'day') { d.setHours(0, 0, 0, 0); d.setDate(d.getDate() - k); }
    else if (period === 'week') { d.setHours(0, 0, 0, 0); d.setDate(d.getDate() - ((d.getDay() + 6) % 7) - 7 * k); }
    else if (period === 'month') { d.setHours(0, 0, 0, 0); d.setDate(1); d.setMonth(d.getMonth() - k); }
    else { d.setHours(0, 0, 0, 0); d.setMonth(0, 1); d.setFullYear(d.getFullYear() - k); }
    ends.push(at(d));
  }
  const span = (i: number): number => {
    if (period === 'hour') return 3600;
    if (period === 'day') return 86400;
    if (period === 'week') return 7 * 86400;
    const d = new Date(ends[i] * 1000);
    if (period === 'month') d.setMonth(d.getMonth() - 1); else d.setFullYear(d.getFullYear() - 1);
    return ends[i] - at(d);
  };
  const out: HistoryRecord[] = [];
  // Lifetime counters at a record's end: the base at today's midnight ± the hours between.
  const mid = new Date(nowMs); mid.setHours(0, 0, 0, 0);
  const midS = at(mid);
  const counter = (t: number, dir: 'in' | 'out'): number => {
    let sum = 0;
    const [a, b] = t >= midS ? [midS, t] : [t, midS];
    for (let t0 = a; t0 < b; t0 += 3600) {
      const d = new Date(t0 * 1000);
      const w = demoHourW(role, d.getHours(), demoDayNo(t0));
      sum += dir === 'in' ? Math.max(0, w) : Math.max(0, -w);
    }
    return (dir === 'in' ? E_IN_BASE : E_OUT_BASE) + (t >= midS ? sum : -sum);
  };
  ends.forEach((end, k) => {
    const nominal = span(k);
    const partial = period === 'hour' && k === 2; // one recent partial hour (a short outage)
    const dur = partial ? Math.round(nominal / 3) : nominal;
    const q = partial ? (1 << 0) : 0;
    // Σ over the bucket's hours (local clock): energy in / out, money, mean / min / max power.
    let ec = 0, es = 0, c = 0, r = 0, pSum = 0, pMin = Infinity, pMax = -Infinity, hours = 0;
    for (let t0 = end - nominal; t0 < end; t0 += 3600) {
      const d = new Date(t0 * 1000);
      const dayNo = demoDayNo(t0);
      const w = demoHourW(role, d.getHours(), dayNo) * (partial ? 1 / 3 : 1);
      const imp = Math.max(0, w), exp = Math.max(0, -w);
      ec += imp; es += exp; c += (imp / 1000) * demoRateAt(d.getHours()); r += (exp / 1000) * demoSellRate();
      pSum += w; pMin = Math.min(pMin, w); pMax = Math.max(pMax, w); hours += 1;
    }
    const pAvg = hours ? pSum / hours : 0;
    const ts = end;
    if (type === 'electrical') {
      const p = Math.abs(pAvg) < 1 ? 0 : pAvg;
      out.push({
        ts, dur, q, res: 1,
        v: jitter(demoRegion().v), i: Math.abs(p) / demoRegion().v, p_act: p,
        s: Math.abs(p) * 1.01, pf: 0.98, f: demoRegion().hz, thd_v: 2.1, thd_i: 3.4, p_rea: Math.abs(p) * 0.1, p_app: Math.abs(p) * 1.01,
        v_min: demoRegion().v - 4, v_max: demoRegion().v + 4, i_min: Math.abs(pMin) / demoRegion().v, i_max: Math.abs(pMax) / demoRegion().v,
        p_min: period === 'hour' ? p * 0.8 : pMin, p_max: period === 'hour' ? p * 1.2 : pMax,
      } as unknown as HistoryRecord);
    } else if (type === 'energy') {
      out.push({ ts, dur, q, res: 1, ec: Math.round(ec), es: Math.round(es), en: Math.round(ec - es), et_in: Math.round(counter(end, 'in')), et_out: Math.round(counter(end, 'out')) });
    } else {
      // Money on the wire = float MAJOR units; a generation line has none of its own.
      const cost = role === 'solar' ? 0 : Math.round(c * 100) / 100;
      const rev = role === 'input' ? Math.round(r * 100) / 100 : 0;
      out.push({ ts, dur, q, res: 0, c: cost, r: rev, n: Math.round((cost - rev) * 100) / 100, cur: demoRegion().cur, plan_id: 1, plan_ver: 3 });
    }
  });
  // One legacy record written before SNTP (uptime-style ts) → "before time sync" group.
  const legacyTs = 86_400; // ~1 day of uptime, well below the 1.6e9 wall-clock floor
  const nominal = period === 'hour' ? 3600 : 86400;
  if (type === 'energy') out.push({ ts: legacyTs, dur: nominal, q: (1 << 1), res: 1, ec: 540, es: 0, en: 540 });
  else if (type === 'economics') out.push({ ts: legacyTs, dur: nominal, q: (1 << 1), res: 0, c: dm(3.47), r: 0, n: dm(3.47), cur: demoRegion().cur, plan_id: 1, plan_ver: 3 });
  else out.push({ ts: legacyTs, dur: nominal, q: (1 << 1), v: 229, i: 2.3, p_act: 540, s: 545, pf: 0.98, f: 50, thd_v: 2, thd_i: 3, v_min: 226, v_max: 233, i_min: 1.8, i_max: 2.8, p_min: 430, p_max: 650 } as unknown as HistoryRecord);
  return out;
}

// ---- Settings demo data ----

export function demoSystemConfig(): SystemConfig {
  return {
    version: '2.0', device_id: 'rbgrid_demo_001', home_name: demoSite().title, location: 'Demo',
    language: 'EN', logging_level: 2, enable_logging: true, enable_telemetry: true,
    auto_start_measurements: true,
  };
}

export function demoWifiStatus(): WifiStatus {
  return {
    mode: 'STA', ssid: 'RBGrid-Demo', rssi: -58, connected: true,
    sta_ip: '192.168.1.42', gateway: '192.168.1.1', netmask: '255.255.255.0',
  };
}

/** The demo hub lives in the VIEWER's zone (POSIX, west-positive: UTC+8 → "<+08>-8") — its charts,
 *  tariff zones and «сейчас» then match the viewer's clock (the history profiles are drawn in it). */
export function demoPosixTz(): string {
  const east = -new Date().getTimezoneOffset();
  const a = Math.abs(east), h = Math.floor(a / 60), m = a % 60;
  const mm = m ? `:${String(m).padStart(2, '0')}` : '';
  return `<${east >= 0 ? '+' : '-'}${String(h).padStart(2, '0')}${m ? String(m).padStart(2, '0') : ''}>${east >= 0 ? '-' : ''}${h}${mm}`;
}

export function demoTimeView(): TimeConfigView {
  return {
    config: {
      enable_time_sync: true, primary_source: 'SNTP', sntp_server: 'pool.ntp.org',
      timezone: demoPosixTz(), enable_dst: false, resync_interval_sec: 3600,
    },
    statistics: { last_sync_age_sec: 42, drift_sec: 0.2 },
  };
}

const DEMO_TARIFF_KEY = 'rbgrid.demo.tariff';
/** Demo tariff save: kept in this browser and read back, like the hub's live apply. */
export function demoSaveTariff(full: BaseResponse<TariffPlanConfigData>): void {
  try { localStorage.setItem(DEMO_TARIFF_KEY, JSON.stringify(full)); } catch { /* storage off */ }
}

const demoDayNow = (): boolean => demoIsDay(new Date().getHours());

export function demoTariffConfig(): BaseResponse<TariffPlanConfigData> {
  try {
    const saved = localStorage.getItem(DEMO_TARIFF_KEY);
    if (saved) return JSON.parse(saved) as BaseResponse<TariffPlanConfigData>;
  } catch { /* fall through to the canned plan */ }
  const tp = demoTariff();
  const plan: TariffPlan = {
    // In the UI language (TAR-21: a Russian plan in the English demo); zone names empty → the
    // zone codes are shown in words by the UI.
    plan_name: tp.plan, provider_name: tp.provider, currency_code: demoRegion().cur,
    plan_type: 'TWO_ZONE', vat_percent: tp.vat, energy_tax_per_kwh: 0,
    net_metering_enabled: tp.sell != null, net_metering_default_ratio: tp.sell != null ? Math.round((tp.sell / tp.day) * 100) / 100 : 0, default_selling_rate_per_kwh: tp.sell ?? 0,
    is_active: true, is_default: true, active_zones_count: 2, default_zone_id: 'NIGHT',
    zones: [
      {
        zone_id: 'DAY', zone_name: '', rate_buying_per_kwh: tp.day, is_enabled: true, is_currently_active: demoDayNow(),
        conditions: [{ condition_type: 'TIME_RANGE', is_active: true, priority: 1, params: { start_hour: tp.dayFrom, start_minute: 0, end_hour: tp.dayTo, end_minute: 0, spans_midnight: false } }],
      },
      {
        zone_id: 'NIGHT', zone_name: '', rate_buying_per_kwh: tp.night, is_enabled: true, is_currently_active: !demoDayNow(),
        conditions: [{ condition_type: 'TIME_RANGE', is_active: true, priority: 1, params: { start_hour: tp.dayTo, start_minute: 0, end_hour: tp.dayFrom, end_minute: 0, spans_midnight: true } }],
      },
    ],
    progressive_config: { is_enabled: false },
    fixed_payments: [{ payment_type: 'STANDING', payment_name: 'Standing charge', amount: tp.fixed, currency: demoRegion().cur, is_enabled: true, billing_period: 'MONTHLY' }],
    fixed_payments_count: 1,
  };
  return demoEnvelope('tariff_config', { list: [plan] });
}

export function demoTelemetryView(): TelemetryConfigView {
  return {
    telemetry_enabled: true, json_buffer_size: 8192, compact_json: true,
    channels: [
      { protocol: 'WEBSOCKET', enabled: true, send_interval_ms: 3000, config: { server_url: '/ws-telemetry' } },
      { protocol: 'MQTT', enabled: false, send_interval_ms: 3000, config: { broker_url: 'mqtt://192.168.1.10', topic: 'rbgrid/telemetry', qos: 0 } },
    ],
  };
}

/** A short hub log for the demo: a planned restart, a clock sync, one module poll failure. */
export function demoHubEvents(level: 'all' | 'event' | 'fail'): HubEventsPage {
  const now = Math.floor(Date.now() / 1000);
  const t0 = now - 3 * 3600;
  const all: HubEvent[] = [
    { seq: 1, b: 1, ts: t0, up: 1, lvl: 'event', msg: 'boot: reset=power-on' },
    { seq: 2, b: 1, ts: t0 + 4, up: 4, lvl: 'event', msg: 'wifi Connected' },
    { seq: 3, b: 1, ts: t0 + 5, up: 5, lvl: 'event', msg: 'fw: image 10723b74' },
    { seq: 4, b: 1, ts: t0 + 3600, up: 3600, lvl: 'event', msg: 'HOUR closed E=812.4Wh' },
    // One poll timeout of the site's last module that cleared by itself (the next polls answer).
    { seq: 5, b: 1, ts: t0 + 5400, up: 5400, lvl: 'fail', msg: `FAIL poll m${Math.max(0, demoSite().modules.length - 1)} timeout` },
    { seq: 6, b: 1, ts: t0 + 7200, up: 7200, lvl: 'event', msg: 'ota: rebooting' },
    { seq: 7, b: 2, ts: t0 + 7230, up: 2, lvl: 'event', msg: 'boot: reset=planned restart' },
    { seq: 8, b: 2, ts: t0 + 7235, up: 7, lvl: 'event', msg: 'ota: new image confirmed valid' },
  ];
  const rank = { periodic: 0, event: 1, fail: 2 } as const;
  const min = level === 'all' ? 0 : rank[level];
  const events = all.filter((e) => rank[e.lvl] >= min);
  return { events, boot: 2, seq_last: 8, next: 8, more: false, persist: true, fail_boot: 0, now };
}

export function demoHubVersion(): HubVersion {
  // No version number: demo.rbgrid.io is a public surface and may only name a published firmware
  // (site team 03.11) — the screens show «—» instead.
  return { project: 'main', idf: 'v5.4.1', build_date: 'Nov 1 2026 12:00:00', partition: 'ota_0', image_md5: '10723b74229b8ec55b881dc239f0897b' };
}
