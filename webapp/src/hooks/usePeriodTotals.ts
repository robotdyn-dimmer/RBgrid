/**
 * Energy and money of a period (Economics), from the hub's CLOSED history records by its clock.
 * `/summary?period=…` without from/to sums EVERY record of that ring (root canon ed876014), and
 * the running week / month / year has no closed record of its own — .110 «за месяц» was two
 * stale 2024 records of 0. So the period is built from the finest closed buckets:
 *   day   = today's closed hours
 *   week  = closed days of the last 7 (before today) + today's hours
 *   month = closed days of this month + today's hours
 *   year  = closed months of this year + this month's days + today's hours
 *   hour  = the last closed hour
 * The running hour is not in any record — the Overview adds it from the live counters.
 */
import { useMemo } from 'react';
import { useHistory, HistoryRange } from './useHistory';
import { useDeviceClock } from '../core/deviceTime';
import { windowSpan, windowTotals, WindowTotals } from '../pages/historyWindow';
import type { SummaryPeriod } from '../core/economics/types';
import type { HistoryRecord } from '../types/history';

export interface PeriodTotals extends WindowTotals {
  loading: boolean;
}

const ZERO: WindowTotals = { inWh: 0, outWh: 0, cost: 0, revenue: 0, n: 0 };
const add = (a: WindowTotals, b: WindowTotals): WindowTotals => ({
  inWh: a.inWh + b.inWh, outWh: a.outWh + b.outWh, cost: a.cost + b.cost, revenue: a.revenue + b.revenue, n: a.n + b.n,
});
/** Energy from the energy family, money from the economics family (their n counts once). */
const merge = (e: WindowTotals, m: WindowTotals): WindowTotals => ({ inWh: e.inWh, outWh: e.outWh, cost: m.cost, revenue: m.revenue, n: Math.max(e.n, m.n) });

/** The last closed record (records are interval starts — see useHistory). */
const lastOf = (recs: HistoryRecord[]): HistoryRecord | null => recs.reduce<HistoryRecord | null>((a, r) => (!a || r.ts > a.ts ? r : a), null);

/** The current periods, plus two closed ones (HA-R11: Economics had no «вчера» / «прошлый месяц»). */
export type TotalsPeriod = SummaryPeriod | 'yesterday' | 'lastMonth';
export const isPastPeriod = (p: TotalsPeriod): boolean => p === 'yesterday' || p === 'lastMonth';

export function usePeriodTotals(unitId: string | null, period: TotalsPeriod, opts: { money?: boolean; today?: WindowTotals | null } = {}): PeriodTotals | null {
  const clock = useDeviceClock();
  const money = opts.money !== false;
  const spans = useMemo(() => {
    const now = Math.floor(Date.now() / 60_000) * 60; // a minute's resolution keeps ranges stable per render
    const today = clock.dayStart(now);
    const monthFrom = windowSpan('thisMonth', now, clock).from;
    const days: HistoryRange | null = period === 'week' ? { from: windowSpan('week', now, clock).from, to: today }
      : period === 'month' || period === 'year' ? { from: monthFrom, to: today }
        // Closed periods: yesterday's day record, last month's days (checked against the hours).
        : period === 'yesterday' ? { from: windowSpan('yesterday', now, clock).from, to: today }
          : period === 'lastMonth' ? { from: windowSpan('lastMonth', now, clock).from, to: monthFrom } : null;
    const months: HistoryRange | null = period === 'year' ? { from: windowSpan('year', now, clock).from, to: monthFrom } : null;
    return { hours: { from: today, to: now + 3600 } as HistoryRange, days, months };
  }, [clock, period]);

  const hE = useHistory(unitId, 'hour', 'energy', 30, spans.hours);
  const hM = useHistory(money ? unitId : null, 'hour', 'economics', 30, spans.hours);
  const dE = useHistory(spans.days ? unitId : null, 'day', 'energy', 40, spans.days ?? undefined);
  const dM = useHistory(spans.days && money ? unitId : null, 'day', 'economics', 40, spans.days ?? undefined);
  const mE = useHistory(spans.months ? unitId : null, 'month', 'energy', 14, spans.months ?? undefined);
  const mM = useHistory(spans.months && money ? unitId : null, 'month', 'economics', 14, spans.months ?? undefined);

  return useMemo(() => {
    if (!unitId) return null;
    const loading = hE.loading || hM.loading || dE.loading || dM.loading || mE.loading || mM.loading;
    if (period === 'hour') {
      const e = lastOf(hE.records);
      const m = e ? hM.records.find((r) => r.ts === e.ts) ?? null : null;
      const one = (r: HistoryRecord | null) => (r ? windowTotals([r], { from: r.ts, to: r.ts + 1 }) : ZERO);
      return { ...merge(one(e), one(m)), loading };
    }
    // Today: the hub's DAY COUNTERS when given (root, joint-hw-testing: «сегодня» = e_day_in /
    // cost_day — they survive restarts; the hour records may miss what a reflash lost), else
    // today's closed hours.
    let total = isPastPeriod(period) ? ZERO
      : opts.today ?? merge(windowTotals(hE.records, spans.hours as Required<HistoryRange>), windowTotals(hM.records, spans.hours as Required<HistoryRange>));
    if (spans.days) total = add(total, merge(windowTotals(dE.records, spans.days as Required<HistoryRange>), windowTotals(dM.records, spans.days as Required<HistoryRange>)));
    if (spans.months) total = add(total, merge(windowTotals(mE.records, spans.months as Required<HistoryRange>), windowTotals(mM.records, spans.months as Required<HistoryRange>)));
    // How far back the data goes: the earliest record of the window with energy in it.
    const inSpan = (rs: HistoryRecord[], sp: HistoryRange | null) => (sp ? rs.filter((r) => r.ts >= (sp.from ?? 0) && r.ts < (sp.to ?? Infinity) && ((r as { ec?: number }).ec ?? 0) > 0) : []);
    const first = [...inSpan(hE.records, spans.hours), ...inSpan(dE.records, spans.days), ...inSpan(mE.records, spans.months)]
      .reduce<number | undefined>((m, r) => (m == null || r.ts < m ? r.ts : m), undefined);
    // No closed record at all but today's counters run (a fresh hub, or a hub without an archive):
    // the data starts today, not on the 1st — a month forecast «as since the 1st» from 16 minutes
    // read ≈ 0,02 € (.191, 17.10).
    const since = first ?? (!loading && total.n > 0 && !isPastPeriod(period) ? spans.hours.from : undefined);
    return { ...total, since, loading };
  }, [unitId, period, spans, opts.today, hE.records, hM.records, dE.records, dM.records, mE.records, mM.records,
    hE.loading, hM.loading, dE.loading, dM.loading, mE.loading, mM.loading]);
}
