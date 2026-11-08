/**
 * Savings insights — pure, testable formulas (review 10 §3; personas 05/06/07/09 «деньги»).
 * Inputs are device data (telemetry, tariff plan, history, summary); outputs are numbers the
 * UI formats in the device currency. All results are ESTIMATES → the UI labels them «≈».
 */
import type { ZoneSpan } from './tariffPlan';
import { t } from '../i18n';

const HOURS_PER_MONTH = 24 * 30;

/** «Сейчас дорого»: the live rate vs the day's cheapest zone; null when not notably dearer. */
export function expensiveNow(
  liveBuy: number | null,
  spans: ZoneSpan[],
  minRatio = 1.2,
): { ratio: number; cheapestRate: number; cheapestName: string } | null {
  if (liveBuy == null) return null;
  const valid = spans.filter((s) => s.rate > 0);
  if (!valid.length) return null;
  const min = valid.reduce((a, b) => (b.rate < a.rate ? b : a));
  const ratio = liveBuy / min.rate;
  return ratio >= minRatio ? { ratio, cheapestRate: min.rate, cheapestName: min.name } : null;
}

/** Cost of the always-on base load per month: min power over a window × hours × rate. */
export function baseLoadMonthly(minW: number | null, avgRate: number | null): { w: number; money: number } | null {
  if (minW == null || avgRate == null || minW <= 0) return null;
  return { w: minW, money: (minW / 1000) * HOURS_PER_MONTH * avgRate };
}

/** Calendar parts of a moment (DeviceClock.parts — the hub's clock). */
export interface CalendarParts { y: number; mo: number; d: number; h: number; mi: number }

/** Linear month-end forecast from the month-to-date cost. `now` = the HUB's calendar parts: the
 *  month-to-date is the hub's month, so its length and the elapsed part are too (Kimi #16: a browser
 *  on the other side of a month boundary forecast with the wrong month). A Date reads local time. */
export function monthForecast(costMtd: number | null, now: CalendarParts | Date, coveredDays?: number | null): { forecast: number; elapsedDays: number; daysInMonth: number; partial: boolean } | null {
  if (costMtd == null || costMtd < 0) return null;
  const p: CalendarParts = now instanceof Date
    ? { y: now.getFullYear(), mo: now.getMonth() + 1, d: now.getDate(), h: now.getHours(), mi: now.getMinutes() } : now;
  const daysInMonth = new Date(Date.UTC(p.y, p.mo, 0)).getUTCDate();
  const calendar = p.d - 1 + (p.h * 60 + p.mi) / 1440;
  // The hub's records may start mid-month (a new install, a wiped archive): the pace is the days
  // WITH data, not the calendar days (3φ audit H3P-01: 1,5 days of data over 15,7 calendar days
  // forecast a bill ≈ 9 × too low).
  const partial = coveredDays != null && coveredDays < calendar - 0.5;
  const elapsedDays = partial ? (coveredDays as number) : calendar;
  if (elapsedDays < (partial ? 1 : 0.25)) return null; // too little to extrapolate honestly
  return { forecast: (costMtd / elapsedDays) * daysInMonth, elapsedDays, daysInMonth, partial };
}

export interface BillShare { name: string; wh: number; share: number; money: number | null; /** the point's key; '' = the unmetered rest */ key?: string }

/**
 * Who costs what today: each line's share of the input's energy today, applied to today's cost.
 * Measured points past the top are «Other points», what no point measures is «Unmetered»; shares
 * sum to 100 %.
 */
export function billShares(
  lines: { name: string; wh: number | null; key?: string }[],
  totalWh: number | null,
  costDay: number | null,
  top = 3,
): BillShare[] {
  if (!totalWh || totalWh <= 0) return [];
  const known = lines.filter((l) => (l.wh ?? 0) > 0).map((l) => ({ name: l.name, wh: l.wh as number, key: l.key }));
  known.sort((a, b) => b.wh - a.wh);
  // Lines can out-sum the input (counter resets, clock skew, a line metered before the input):
  // never report shares > 100 % — renormalize to the lines' sum, and there's no «Прочее» then.
  const linesWh = known.reduce((s, l) => s + l.wh, 0);
  if (linesWh > totalWh) totalWh = linesWh;
  const toShare = (name: string, wh: number, key?: string): BillShare => {
    const share = Math.max(0, Math.min(1, wh / totalWh));
    return { name, wh, share, money: costDay != null ? share * costDay : null, key };
  };
  const head = known.slice(0, top).map((l) => toShare(l.name, l.wh, l.key));
  // The remainder in two honest parts: measured points past the top («Other points») and what no
  // point measures («Unmetered») — one «Other» read as full coverage when it was the unmetered rest
  // (site team 02.11).
  const others = known.slice(top).reduce((s, l) => s + l.wh, 0);
  const unmetered = Math.max(0, totalWh - linesWh);
  if (others > totalWh * 0.01) head.push(toShare(t('economics.otherShare'), others, ''));
  if (unmetered > totalWh * 0.01) head.push(toShare(t('economics.unmeteredShare'), unmetered, ''));
  return head;
}

/** Today vs yesterday, comparing today's run-rate (projected to a full day) with yesterday. */
export function vsYesterday(todayCost: number | null, yesterdayCost: number | null, now: Date): { pct: number; projected: number } | null {
  if (todayCost == null || yesterdayCost == null || yesterdayCost <= 0) return null;
  const dayFrac = (now.getHours() * 60 + now.getMinutes()) / 1440;
  if (dayFrac < 0.25) return null; // morning run-rate is meaningless
  const projected = todayCost / dayFrac;
  const pct = (projected / yesterdayCost - 1) * 100;
  // A ×4+ jump vs yesterday is almost always a data artefact (reset, partial day) — don't advise on it.
  if (Math.abs(pct) > 300) return null;
  return { pct, projected };
}

/**
 * «Зарядка в дорогой зоне» (site brief 02.11): a past day's charging energy by the hour
 * (hub-local hour 0–23 → Wh) priced by that day's zones — the energy that ran in zones dearer than
 * the cheapest, and what it cost above the cheapest zone. A fact from the plan, not a forecast.
 * Null when nothing ran in a dearer zone (or the plan has one rate).
 */
export function dearZoneCharge(
  whByHour: ReadonlyMap<number, number>,
  spans: ZoneSpan[],
  minWh = 1000,
): { dearWh: number; extra: number; zone: string; cheapestName: string } | null {
  const valid = spans.filter((s) => s.rate > 0);
  if (!valid.length) return null;
  const min = valid.reduce((a, b) => (b.rate < a.rate ? b : a));
  let dearWh = 0, extra = 0;
  const byZone = new Map<string, number>();
  whByHour.forEach((wh, h) => {
    const m = h * 60 + 30;
    const sp = valid.find((s) => m >= s.from && m < s.to);
    if (!sp || wh <= 0 || sp.rate - min.rate < 1e-9) return;
    dearWh += wh;
    extra += (wh / 1000) * (sp.rate - min.rate);
    byZone.set(sp.name, (byZone.get(sp.name) ?? 0) + wh);
  });
  if (dearWh < minWh || extra <= 0) return null;
  const zone = Array.from(byZone.entries()).sort((a, b) => b[1] - a[1])[0][0];
  return { dearWh, extra, zone, cheapestName: min.name };
}
