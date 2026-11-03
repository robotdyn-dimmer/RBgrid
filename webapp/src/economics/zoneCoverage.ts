/**
 * Which tariff zone is in force at every minute of the WEEK, by the hub's own rules (UX audit
 * W2-3: TAR-03/18 + weekdays). Mirrors firmware `pro/rbhome_tariff.c`:
 *  - a zone's conditions are AND-ed (TIME_RANGE × WEEKDAY × WEEKEND) unless `conditions_and_logic`
 *    is false (OR); no conditions = always;
 *  - `weekday_mask` is Monday-origin: bit0 = Mon … bit6 = Sun;
 *  - several zones may match — the zone TYPE decides (CRITICAL_PEAK 100 > HOLIDAY 90 > WEEKEND
 *    80 on Sat/Sun (20 on weekdays) > EVENING_PEAK 70 > MORNING_PEAK 65 > NIGHT 60 >
 *    LATE_EVENING 55 > DAY 50 > other 10), so an overlap is a rule, not an error;
 *  - a minute no zone matches falls back to the plan's `default_zone_id`.
 * Zones with calendar conditions (HOLIDAY, DATE_RANGE, SEASON, MONTH, SPECIAL_EVENT) act on
 * special days only and are left out of the ordinary week.
 */
import type { TariffZone } from '../types/tariff-plan';

export interface TimeRange { start: number; end: number } // minutes 0..1440, end exclusive; end ≤ start = across midnight

export const ALL_DAYS = 0x7f;
export const WEEKEND_DAYS = 0x60; // Sat (bit5) + Sun (bit6)
/** Monday-origin day index (0 = Mon … 6 = Sun) of a JS weekday (0 = Sun). */
export const monIdx = (jsDay: number): number => (jsDay + 6) % 7;

export function zoneRange(z: TariffZone): TimeRange | null {
  const c = (z.conditions ?? []).find((x) => x.condition_type === 'TIME_RANGE');
  if (!c) return null;
  const p = c.params as Record<string, number>;
  const start = (p.start_hour ?? 0) * 60 + (p.start_minute ?? 0);
  const end = (p.end_hour ?? 0) * 60 + (p.end_minute ?? 0);
  return { start, end };
}

/** Days the zone applies on (Mon-origin mask). */
export function zoneDays(z: TariffZone): number {
  let mask = ALL_DAYS;
  for (const c of z.conditions ?? []) {
    if (c.is_active === false) continue;
    if (c.condition_type === 'WEEKDAY') mask &= Number((c.params as Record<string, unknown>)?.weekday_mask ?? ALL_DAYS) & ALL_DAYS;
    else if (c.condition_type === 'WEEKEND') mask &= WEEKEND_DAYS;
  }
  return mask;
}

const ORDINARY = new Set(['TIME_RANGE', 'WEEKDAY', 'WEEKEND']);
/** The zone can win on an ordinary day (no calendar-only condition). */
const ordinary = (z: TariffZone): boolean => (z.conditions ?? []).every((c) => c.is_active === false || ORDINARY.has(c.condition_type));

/** Firmware priority of a zone type (find_active_tariff_zone). */
export function zonePriority(zoneId: string | number, weekend: boolean): number {
  switch (String(zoneId)) {
    case 'CRITICAL_PEAK': return 100;
    case 'HOLIDAY': return 90;
    case 'WEEKEND': return weekend ? 80 : 20;
    case 'EVENING_PEAK': return 70;
    case 'MORNING_PEAK': return 65;
    case 'NIGHT': return 60;
    case 'LATE_EVENING': return 55;
    case 'DAY': return 50;
    default: return 10;
  }
}

function inRange(r: TimeRange | null, m: number): boolean {
  if (!r) return true;
  if (r.end > r.start) return m >= r.start && m < r.end;
  if (r.end === r.start) return true; // 00:00–00:00 = the whole day
  return m >= r.start || m < r.end;
}

/** Conditions AND-ed (the hub's default) or OR-ed when `conditions_and_logic: false` (canon 54fffc83). */
function zoneMatches(z: TariffZone, day: number, minute: number): boolean {
  const active = (z.conditions ?? []).filter((c) => c.is_active !== false);
  if (!active.length) return true;
  const dayOk = (zoneDays(z) & (1 << day)) !== 0;
  const timeOk = inRange(zoneRange(z), minute);
  if (z.conditions_and_logic !== false) return dayOk && timeOk;
  const hasTime = active.some((c) => c.condition_type === 'TIME_RANGE');
  const hasDays = active.some((c) => c.condition_type === 'WEEKDAY' || c.condition_type === 'WEEKEND');
  return (hasTime && timeOk) || (hasDays && dayOk);
}

/** The zones matching (day 0 = Mon, minute): the winner by type, and the ones it overrides. */
export function hitsAt(zones: TariffZone[], day: number, minute: number): { win: number; losers: number[] } {
  const weekend = day >= 5;
  let win = -1;
  let bp = 0;
  const hits: number[] = [];
  zones.forEach((z, i) => {
    if (!z.is_enabled || !ordinary(z) || !zoneMatches(z, day, minute)) return;
    hits.push(i);
    const p = zonePriority(z.zone_id, weekend);
    if (p > bp) { bp = p; win = i; } // equal priority: the first zone wins (strict >, as on the hub)
  });
  return { win, losers: hits.filter((i) => i !== win) };
}

/** Index of the zone that matches (day, minute) — null when none does (the default zone then). */
export function matchZone(zones: TariffZone[], day: number, minute: number): number | null {
  const { win } = hitsAt(zones, day, minute);
  return win >= 0 ? win : null;
}

/** Index of the plan's default zone (null when the plan has none among its zones). */
export function defaultZoneIndex(zones: TariffZone[], defaultZoneId: string | number | undefined): number | null {
  if (defaultZoneId === undefined || defaultZoneId === null || defaultZoneId === '') return null;
  const i = zones.findIndex((z) => String(z.zone_id) === String(defaultZoneId));
  return i >= 0 ? i : null;
}

/** A run of one state over consecutive minutes on a set of days. */
export interface WeekSpan extends TimeRange { days: number }
export interface Shadowed { zone: number; by: number[] }

export interface Coverage {
  /** Minutes with no matching zone, grouped by identical days (the default zone takes them). */
  gaps: WeekSpan[];
  /** The zone the gaps fall to (null = no default → the energy is unpriced). */
  fallback: number | null;
  /** Spans where several zones match, with the winner (informational: the hub decides by type). */
  overlaps: (WeekSpan & { winner: number; losers: number[] })[];
  /** Enabled ordinary zones that never win anywhere in the week. */
  shadowed: Shadowed[];
  /** Winner per day (0 = Mon) per minute; -1 = no zone, else the zone index (fallback applied). */
  week: Int16Array[];
}

/** Group per-day runs with identical (start, end, payload) across days. */
function groupDays<T extends { start: number; end: number; key: string }>(perDay: T[][]): (T & { days: number })[] {
  const map = new Map<string, T & { days: number }>();
  perDay.forEach((runs, d) => runs.forEach((r) => {
    const k = `${r.start}-${r.end}-${r.key}`;
    const ex = map.get(k);
    if (ex) ex.days |= 1 << d; else map.set(k, { ...r, days: 1 << d });
  }));
  return Array.from(map.values()).sort((a, b) => a.start - b.start || a.days - b.days);
}

export function zoneCoverage(zones: TariffZone[], defaultZoneId?: string | number): Coverage {
  const fallback = defaultZoneIndex(zones, defaultZoneId);
  const week: Int16Array[] = [];
  const gapRuns: { start: number; end: number; key: string }[][] = [];
  const ovRuns: { start: number; end: number; key: string; winner: number; losers: number[] }[][] = [];
  const won = new Set<number>();
  const anyRange = zones.some((z) => z.is_enabled && ordinary(z) && zoneRange(z));
  for (let d = 0; d < 7; d++) {
    const row = new Int16Array(1440);
    const gaps: { start: number; end: number; key: string }[] = [];
    const ovs: { start: number; end: number; key: string; winner: number; losers: number[] }[] = [];
    let gs = -1;
    let ov: { start: number; key: string; winner: number; losers: number[] } | null = null;
    for (let m = 0; m <= 1440; m++) {
      let win = -1;
      let losers: number[] = [];
      if (m < 1440) {
        ({ win, losers } = hitsAt(zones, d, m));
        if (win >= 0) won.add(win);
        row[m] = win >= 0 ? win : fallback ?? -1;
      }
      const isGap = m < 1440 && win < 0;
      if (isGap && gs < 0) gs = m;
      if (!isGap && gs >= 0) { gaps.push({ start: gs, end: m, key: '' }); gs = -1; }
      const key = losers.length ? `${win}:${losers.join(',')}` : '';
      if (ov && ov.key !== key) { ovs.push({ start: ov.start, end: m, key: ov.key, winner: ov.winner, losers: ov.losers }); ov = null; }
      if (!ov && key) ov = { start: m, key, winner: win, losers };
    }
    week.push(row);
    gapRuns.push(gaps);
    ovRuns.push(ovs);
  }
  if (fallback !== null) won.add(fallback);
  // A plan without any time range (one flat rate) has no coverage question.
  const gaps = anyRange ? groupDays(gapRuns).map(({ key: _k, ...g }) => g) : [];
  const overlaps = groupDays(ovRuns).map(({ key: _k, ...o }) => o);
  const shadowed: Shadowed[] = [];
  zones.forEach((z, i) => {
    if (!z.is_enabled || !ordinary(z) || won.has(i)) return;
    const by = new Set<number>();
    overlaps.forEach((o) => { if (o.losers.includes(i)) by.add(o.winner); });
    shadowed.push({ zone: i, by: Array.from(by) });
  });
  return { gaps, fallback, overlaps, shadowed, week };
}

/** Days mask → «Пн–Пт», «Сб, Вс», «каждый день» (labels from the caller: Mon-first short names). */
export function daysText(mask: number, short: string[], everyDay: string): string {
  if ((mask & ALL_DAYS) === ALL_DAYS) return everyDay;
  const on = Array.from({ length: 7 }, (_, d) => (mask >> d) & 1);
  const parts: string[] = [];
  for (let d = 0; d < 7; d++) {
    if (!on[d]) continue;
    let e = d;
    while (e + 1 < 7 && on[e + 1]) e++;
    parts.push(e - d >= 2 ? `${short[d]}–${short[e]}` : e > d ? `${short[d]}, ${short[e]}` : short[d]);
    d = e;
  }
  return parts.join(', ');
}

export const hhmm = (min: number): string => (min >= 1440 ? '24:00' : `${String(Math.floor(min / 60)).padStart(2, '0')}:${String(min % 60).padStart(2, '0')}`);
export const parseHhmm = (s: string): number | null => {
  const m = /^(\d{1,2}):(\d{2})$/.exec(s.trim());
  if (!m) return null;
  const h = Number(m[1]), mi = Number(m[2]);
  return h <= 24 && mi < 60 ? Math.min(1440, h * 60 + mi) : null;
};
