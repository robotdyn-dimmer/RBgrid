/**
 * History windows (UX audit W2-1: RES-05/06). The reader asks «вчера», «прошлый месяц» — a
 * WINDOW of time — and the step follows from it (hours for a day, days for a month, months for a
 * year). The old switch set only the bucket size, so «вчера» could not be picked at all and
 * «Неделя» was empty on a fresh hub. Windows are in the hub's time (core/deviceTime).
 */
import type { HistoryPeriod, HistoryRecord } from '../types/history';
import type { DeviceClock } from '../core/deviceTime';

export type HistWindow = 'today' | 'yesterday' | 'week' | 'thisMonth' | 'lastMonth' | 'year';

export const HIST_WINDOWS: { value: HistWindow; label: string }[] = [
  { value: 'today', label: 'history.winToday' },
  { value: 'yesterday', label: 'history.winYesterday' },
  { value: 'week', label: 'history.winWeek' },
  { value: 'thisMonth', label: 'history.winThisMonth' },
  { value: 'lastMonth', label: 'history.winLastMonth' },
  { value: 'year', label: 'history.winYear' },
];

export interface Span { from: number; to: number }
export interface WindowSpan extends Span {
  step: HistoryPeriod;
  /** The window to compare with (same length, one period back). */
  prev: Span;
  /** Phrase key naming `prev` («вчера к этому часу»…). */
  prevLabel: string;
  /** The whole previous period — drawn on the chart as a ghost (charts audit C3); `prev` (to the
   *  same point in time) stays the basis of the «±N %» line. */
  prevFull?: Span;
  /** Records the window may hold (the request limit). */
  limit: number;
}

const DAY = 86400;

/** Device-local midnight of the 1st of month `mo` (1..12) of year `y`. */
function monthStart(clock: DeviceClock, y: number, mo: number): number {
  const yy = y + Math.floor((mo - 1) / 12);
  const mm = ((((mo - 1) % 12) + 12) % 12);
  return clock.dayStart(Date.UTC(yy, mm, 1, 12) / 1000);
}

export function windowSpan(w: HistWindow, nowSec: number, clock: DeviceClock): WindowSpan {
  const today = clock.dayStart(nowSec);
  const p = clock.parts(nowSec);
  // The comparison covers exactly what the current window holds in CLOSED records (rerun 16.10
  // RES-R03/R04): hours up to the running hour, days up to today, months up to this month —
  // never a part of the period the current side can't have yet.
  const hourNow = today + Math.floor((nowSec - today) / 3600) * 3600;
  switch (w) {
    case 'yesterday': {
      const from = clock.dayStart(today - DAY / 2);
      return { from, to: today, step: 'hour', prev: { from: clock.dayStart(from - DAY / 2), to: from }, prevFull: { from: clock.dayStart(from - DAY / 2), to: from }, prevLabel: 'history.prevDayBefore', limit: 30 };
    }
    case 'week': {
      const from = clock.dayStart(today - 6 * DAY + DAY / 2);
      return { from, to: nowSec, step: 'day', prev: { from: from - 7 * DAY, to: today - 7 * DAY }, prevFull: { from: from - 7 * DAY, to: from }, prevLabel: 'history.prevWeek', limit: 10 };
    }
    case 'thisMonth': {
      const from = monthStart(clock, p.y, p.mo);
      const pFrom = monthStart(clock, p.y, p.mo - 1);
      return { from, to: nowSec, step: 'day', prev: { from: pFrom, to: Math.min(from, pFrom + (today - from)) }, prevFull: { from: pFrom, to: from }, prevLabel: 'history.prevMonthToDate', limit: 40 };
    }
    case 'lastMonth': {
      const from = monthStart(clock, p.y, p.mo - 1);
      const to = monthStart(clock, p.y, p.mo);
      return { from, to, step: 'day', prev: { from: monthStart(clock, p.y, p.mo - 2), to: from }, prevFull: { from: monthStart(clock, p.y, p.mo - 2), to: from }, prevLabel: 'history.prevMonthBefore', limit: 40 };
    }
    case 'year': {
      const from = monthStart(clock, p.y, 1);
      const pFrom = monthStart(clock, p.y - 1, 1);
      return { from, to: nowSec, step: 'month', prev: { from: pFrom, to: Math.min(from, monthStart(clock, p.y - 1, p.mo)) }, prevFull: { from: pFrom, to: from }, prevLabel: 'history.prevYearToDate', limit: 14 };
    }
    case 'today':
    default:
      return { from: today, to: nowSec, step: 'hour', prev: { from: today - DAY, to: hourNow - DAY }, prevFull: { from: today - DAY, to: today }, prevLabel: 'history.prevYesterdaySameTime', limit: 30 };
  }
}

export interface WindowTotals {
  /** Wh taken / given (energy family). */
  inWh: number;
  outWh: number;
  /** Money cost / revenue (economics family). */
  cost: number;
  revenue: number;
  /** Records counted. */
  n: number;
  /** Start of the earliest record counted (unix s) — how far back the data really goes. */
  since?: number;
}

const num = (r: HistoryRecord, k: string): number => {
  const v = (r as unknown as Record<string, unknown>)[k];
  return typeof v === 'number' && Number.isFinite(v) ? v : 0;
};

/** Σ over the records that START inside the span. */
export function windowTotals(records: HistoryRecord[], span: Span): WindowTotals {
  const out: WindowTotals = { inWh: 0, outWh: 0, cost: 0, revenue: 0, n: 0 };
  for (const r of records) {
    if (typeof r.ts !== 'number' || r.ts < span.from || r.ts >= span.to) continue;
    out.inWh += num(r, 'ec');
    out.outWh += num(r, 'es');
    out.cost += num(r, 'c');
    out.revenue += num(r, 'r');
    out.n += 1;
  }
  return out;
}

/** Change vs the comparison window, % (null when there is nothing to compare with). */
export function deltaPct(cur: number, prev: number): number | null {
  if (!Number.isFinite(cur) || !Number.isFinite(prev) || prev <= 0) return null;
  return ((cur - prev) / prev) * 100;
}

/**
 * The window `back` periods ago (rerun 16.10 HA-R10/R11: presets only, no «‹ ›» like HA's
 * energy date picker). back = 0 → windowSpan as is (running, compared to the same part of the
 * period before). back ≥ 1 → that WHOLE past period (a day / 7 days / a month / a year), compared
 * to the one before it.
 */
export function windowSpanAt(w: HistWindow, back: number, nowSec: number, clock: DeviceClock): WindowSpan & { label: { from: number; to: number; kind: 'day' | 'week' | 'month' | 'year' } } {
  const base = windowSpan(w, nowSec, clock);
  const kind = w === 'today' || w === 'yesterday' ? 'day' : w === 'week' ? 'week' : w === 'year' ? 'year' : 'month';
  if (back <= 0) return { ...base, label: { from: base.from, to: Math.min(base.to, nowSec), kind } };
  const p = clock.parts(base.from + 3600);
  let from: number;
  let to: number;
  let pFrom: number;
  if (kind === 'day') {
    from = clock.dayStart(base.from - back * DAY + DAY / 2);
    to = clock.dayStart(from + DAY + DAY / 2);
    pFrom = clock.dayStart(from - DAY / 2);
  } else if (kind === 'week') {
    from = clock.dayStart(base.from - back * 7 * DAY + DAY / 2);
    to = clock.dayStart(from + 7 * DAY + DAY / 2);
    pFrom = clock.dayStart(from - 7 * DAY + DAY / 2);
  } else if (kind === 'month') {
    from = monthStart(clock, p.y, p.mo - back);
    to = monthStart(clock, p.y, p.mo - back + 1);
    pFrom = monthStart(clock, p.y, p.mo - back - 1);
  } else {
    from = monthStart(clock, p.y - back, 1);
    to = monthStart(clock, p.y - back + 1, 1);
    pFrom = monthStart(clock, p.y - back - 1, 1);
  }
  return { ...base, from, to, prev: { from: pFrom, to: from }, prevFull: { from: pFrom, to: from }, prevLabel: 'history.prevPeriod', label: { from, to, kind } };
}
