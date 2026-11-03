/**
 * Home vampires — the standby floor of the house (documents/design/HOME_VAMPIRES.md). Pure
 * helpers over the hub's hour records: the floor is the MEDIAN over nights of each night's median
 * hourly mean between 01:00 and 05:00 (hub time) — one random dip no longer sets it (the old
 * insight took the lowest instantaneous minimum of a day).
 */
import type { HistoryRecord } from '../types/history';
import type { DeviceClock } from '../core/deviceTime';
import type { TariffZone } from '../types/tariff-plan';
import { zoneCoverage } from './zoneCoverage';

const num = (r: HistoryRecord, k: string): number | null => {
  const v = (r as unknown as Record<string, unknown>)[k];
  return typeof v === 'number' && Number.isFinite(v) ? v : null;
};
const median = (xs: number[]): number => {
  const a = [...xs].sort((x, y) => x - y);
  const m = Math.floor(a.length / 2);
  return a.length % 2 ? a[m] : (a[m - 1] + a[m]) / 2;
};

export interface StandbyFloor {
  /** The floor, W (median over the qualifying nights). */
  w: number;
  /** Nights used / needed (3). */
  nights: number;
  /** Change of the last 3 nights against the earlier ones, W (null without enough nights). */
  trendW: number | null;
}

/**
 * Hour records (electrical, interval starts) → the standby floor. Hours 01:00–05:00 of the hub's
 * clock; a night with export (negative mean) is skipped (the input no longer shows the house);
 * a night whose median is > 3 × the overall median is atypical (a night wash) and skipped.
 */
export function standbyFloor(hours: HistoryRecord[], clock: DeviceClock, minNights = 3): StandbyFloor | { w: null; nights: number; trendW: null } {
  const byNight = new Map<number, number[]>();
  for (const r of hours) {
    if (r.ts < 1_600_000_000) continue;
    const p = num(r, 'p_act');
    if (p == null) continue;
    const h = clock.parts(r.ts).h;
    if (h < 1 || h >= 5) continue;
    const night = clock.dayStart(r.ts);
    const list = byNight.get(night) ?? [];
    list.push(p);
    byNight.set(night, list);
  }
  let nights = Array.from(byNight.entries())
    .filter(([, ps]) => ps.length >= 2 && ps.every((p) => p >= 0))
    .map(([d, ps]) => ({ d, w: median(ps) }))
    .sort((a, b) => a.d - b.d);
  if (nights.length) {
    const all = median(nights.map((n) => n.w));
    nights = nights.filter((n) => n.w <= Math.max(3 * all, all + 50));
  }
  if (nights.length < minNights) return { w: null, nights: nights.length, trendW: null };
  const w = median(nights.map((n) => n.w));
  let trendW: number | null = null;
  if (nights.length >= 5) {
    const recent = median(nights.slice(-3).map((n) => n.w));
    const before = median(nights.slice(0, -3).map((n) => n.w));
    trendW = recent - before;
  }
  return { w, nights: nights.length, trendW };
}

/** Average buying price over a week of the plan (a standby load runs round the clock). */
export function weekAverageRate(zones: TariffZone[], defaultZoneId?: string | number): number | null {
  const cov = zoneCoverage(zones, defaultZoneId);
  let sum = 0;
  let n = 0;
  for (const row of cov.week) {
    for (let m = 0; m < 1440; m++) {
      const z = row[m] >= 0 ? zones[row[m]] : null;
      if (z && z.rate_buying_per_kwh > 0) { sum += z.rate_buying_per_kwh; n += 1; }
    }
  }
  return n ? sum / n : null;
}

/** W round the clock → kWh a month (730 h). */
export const monthlyKwh = (w: number): number => (w * 730) / 1000;

/** A rise worth saying: more than max(2 W, 10 %). */
export const isNotableRise = (trendW: number | null, w: number): boolean => trendW != null && trendW > Math.max(2, 0.1 * w);
