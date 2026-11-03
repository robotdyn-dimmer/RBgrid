/**
 * Tariff-plan timeline — pure helpers over the device's tariff plan (GET tariff-plan-config).
 * Expands the zones into one day's timeline (days, priorities and the default zone as on the hub)
 * to answer «до смены», «насколько сейчас дороже самой дешёвой зоны». Calendar conditions
 * (holiday/season/…) are not modelled — the LIVE zone/rate always comes from telemetry system.tariff.
 */
import type { TariffPlan } from '../types/tariff-plan';
import { tCode } from '../i18n';
import { zoneCoverage } from './zoneCoverage';

/**
 * A zone's name for the UI: the user's own name when they set one, else the zone code in the
 * UI language. The hub's factory names are English («Night», «Day») — shown as «Ночь», «День»
 * (UX audit HOME-11/TAR-12: «дороже зоны «Night»» next to «Зона: День»).
 */
export function zoneDisplayName(zoneId: unknown, zoneName: unknown): string {
  const id = typeof zoneId === 'string' ? zoneId : '';
  const name = typeof zoneName === 'string' ? zoneName.trim() : '';
  const localized = id ? tCode('zone', id) : '';
  if (!name) return localized || id;
  const factory = name.toUpperCase().replace(/[\s-]+/g, '_');
  return factory === id || factory === `${id}_TARIFF` ? (localized || name) : name;
}

export interface ZoneSpan {
  zoneId: string;
  name: string;
  /** Minutes from midnight, [from, to). */
  from: number;
  to: number;
  rate: number;
}

/**
 * The zones of one day as the hub prices it (`day` 0 = Mon … 6 = Sun; default Monday): overlaps
 * resolved by zone type, day conditions applied, gaps given to the default zone — the same model
 * as the Tariffs week picture (economics/zoneCoverage).
 */
export function zoneTimeline(plan: TariffPlan | null | undefined, day = 0): ZoneSpan[] {
  const zones = plan?.zones ?? [];
  if (!zones.length) return [];
  const row = zoneCoverage(zones, plan?.default_zone_id).week[day] ?? new Int16Array(1440).fill(-1);
  const out: ZoneSpan[] = [];
  for (let m = 0; m < 1440; m++) {
    const zi = row[m];
    const last = out[out.length - 1];
    if (zi < 0) continue;
    const z = zones[zi];
    if (last && last.to === m && last.zoneId === String(z.zone_id ?? '')) { last.to = m + 1; continue; }
    out.push({ zoneId: String(z.zone_id ?? ''), name: zoneDisplayName(z.zone_id, z.zone_name), rate: z.rate_buying_per_kwh ?? 0, from: m, to: m + 1 });
  }
  return out;
}

/** Minutes until the current zone changes (null when unknown). */
export function minutesToZoneChange(spans: ZoneSpan[], now: Date | { h: number; mi: number }): number | null {
  // The hub switches zones by ITS clock — pass device-local {h, mi} (core/deviceTime, INP-11).
  const m = now instanceof Date ? now.getHours() * 60 + now.getMinutes() : now.h * 60 + now.mi;
  const cur = spans.find((s) => m >= s.from && m < s.to);
  if (!cur) return null;
  // Merge contiguous spans of the same zone (e.g. 23:00–24:00 + 00:00–07:00).
  let end = cur.to;
  if (end === 24 * 60) {
    const wrap = spans.find((s) => s.from === 0 && s.zoneId === cur.zoneId);
    if (wrap) return 24 * 60 - m + wrap.to;
  }
  return end - m;
}

/** The cheapest rate across the day's zones (null when the plan has none). */
export function cheapestRate(spans: ZoneSpan[]): { rate: number; name: string } | null {
  const valid = spans.filter((s) => s.rate > 0);
  if (!valid.length) return null;
  const min = valid.reduce((a, b) => (b.rate < a.rate ? b : a));
  return { rate: min.rate, name: min.name };
}

/** Minutes until the next span priced at `rate` (the cheapest zone) STARTS — today, else
 *  tomorrow's first one (rerun 16.10 TAR-R04: «Дешевле в зоне «Ночь» — смена через 7 ч» counted
 *  to the next change of ANY zone, i.e. to the evening peak). 0 when it is running now. */
export function minutesUntilRate(spans: ZoneSpan[], now: { h: number; mi: number }, rate: number): number | null {
  const m = now.h * 60 + now.mi;
  const cheap = spans.filter((s) => Math.abs(s.rate - rate) < 1e-9);
  if (!cheap.length) return null;
  if (cheap.some((s) => m >= s.from && m < s.to)) return 0;
  const later = cheap.filter((s) => s.from > m).sort((a, b) => a.from - b.from)[0];
  if (later) return later.from - m;
  const first = [...cheap].sort((a, b) => a.from - b.from)[0];
  return 24 * 60 - m + first.from;
}
