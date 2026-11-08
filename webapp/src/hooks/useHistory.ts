/**
 * useHistory — fetch a page of statistics history for a unit/period/type.
 * Records come newest-first; the chart reverses for an ascending time axis.
 */
import { useCallback, useEffect, useRef, useState } from 'react';
import { getApiClient } from '../services/api';
import { isDemo } from '../services/connection';
import {
  HistoryRecord, HistoryPeriod, HistoryType, HistoryAll,
  HistoryElectricalRecord, HistoryEnergyRecord, HistoryEconomicsRecord,
} from '../types/history';
import { demoHistory } from '../mock/demo';
import { toIntervalStarts, wireRange, RecordCalendar } from '../core/time/recordTime';
import { reconcileDays } from '../core/history/reconcileDays';
import { useDeviceClock } from '../core/deviceTime';

/** Interval-start records inside [from, to) (see core/time/recordTime: `ts` on the wire = END). */
function inWindow<T extends HistoryRecord>(recs: T[], period: HistoryPeriod, range: HistoryRange | undefined, cal: RecordCalendar): T[] {
  // An EXACT repeat of a record (same end, duration and values — .139 held one day 4× after clock
  // tests) is one record, not four: summing them made «С начала месяца» jump 2,61 → 8,18 €.
  const seen = new Set<string>();
  const uniq = recs.filter((r) => {
    const k = JSON.stringify(r);
    if (seen.has(k)) return false;
    seen.add(k);
    return true;
  });
  // A record that STARTS after now is a clock artefact (a restart with the wrong time), not data —
  // it drew a «23 h» bar at noon (3φ rerun R02, L3P-22). 2 min of slack for clock skew.
  const nowS = Date.now() / 1000 + 120;
  return toIntervalStarts(uniq, period, cal).filter((r) => r.ts <= nowS && (range?.from === undefined || r.ts >= range.from) && (range?.to === undefined || r.ts < range.to));
}

export interface UseHistoryResult {
  records: HistoryRecord[];
  loading: boolean;
  error: string | null;
  hasMore: boolean;
  reload: () => void;
}

export interface HistoryRange {
  /** Window start (unix seconds). */
  from?: number;
  /** Window end (unix seconds). */
  to?: number;
}

/**
 * Charts re-read the hub's history while the tab is visible (operator 20.10: «графики без
 * автообновления» — the closed hour only appeared on reload, and the running-hour bar, fed live
 * from the totalizers, dropped out at the hour change until then). Quiet: no spinner, the old
 * series stays until the new one lands. A record closes once per period, so the cadence follows
 * it — hours every minute, days every 5 min, longer periods every 15 min — with a little jitter
 * so a page of charts doesn't hit the hub in one burst.
 */
export const HISTORY_REFRESH_MS: Record<HistoryPeriod, number> = {
  hour: 60_000, day: 300_000, week: 900_000, month: 900_000, year: 900_000,
};

/** Run `fn(true)` every `ms` while the page is visible, and once on returning to it. */
export function useQuietRefresh(fn: (quiet: boolean) => unknown, enabled = true, ms: number = HISTORY_REFRESH_MS.hour): void {
  useEffect(() => {
    if (!enabled) return undefined;
    const tick = () => { if (typeof document === 'undefined' || document.visibilityState !== 'hidden') void fn(true); };
    const id = setInterval(tick, ms + Math.floor(Math.random() * 5000));
    const onVis = () => { if (document.visibilityState === 'visible') void fn(true); };
    document.addEventListener('visibilitychange', onVis);
    return () => { clearInterval(id); document.removeEventListener('visibilitychange', onVis); };
  }, [fn, enabled, ms]);
}

export function useHistory(
  unitId: string | null,
  period: HistoryPeriod,
  type: HistoryType,
  limit = 200,
  range?: HistoryRange
): UseHistoryResult {
  const [records, setRecords] = useState<HistoryRecord[]>([]);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [hasMore, setHasMore] = useState(false);
  const mounted = useRef(true);
  // Only the latest request may write: a slow answer for the previous point/period must not
  // land over the new one (Kimi #4). `mounted` alone is re-armed by the next effect.
  const gen = useRef(0);
  const clock = useDeviceClock();

  const load = useCallback(async (quiet = false) => {
    const my = ++gen.current;
    const live = () => mounted.current && gen.current === my;
    if (!unitId) {
      setRecords([]);
      return;
    }
    if (isDemo()) {
      // The demo series honours the window like the hub does.
      if (live()) { setRecords(inWindow(demoHistory(type, period, Math.min(200, Math.max(48, limit)), unitId), period, range, clock)); setHasMore(false); }
      return;
    }
    if (!quiet) { setLoading(true); setError(null); }
    try {
      const res = await getApiClient().getHistory({
        unit_id: unitId, period, type, offset: 0, limit,
        ...wireRange(range?.from, range?.to, period),
      });
      if (!live()) return;
      let recs = inWindow(res.data ?? [], period, range, clock);
      // Day totals are checked against the hub's own hour records where the hour archive still
      // reaches (RES-R01: a day closed with 0 next to 5,8 kWh in its hours).
      if (period === 'day' && (type === 'energy' || type === 'economics')) {
        try {
          const hrs = await getApiClient().getHistory({ unit_id: unitId, period: 'hour', type, offset: 0, limit: 120, ...wireRange(range?.from, range?.to, 'hour') });
          if (!live()) return;
          recs = inWindow(reconcileDays(recs, toIntervalStarts(hrs.data ?? [], 'hour'), (ts) => clock.dayStart(ts)), period, range, clock);
        } catch { /* the hour archive is optional — keep the day records */ }
      }
      setRecords(recs);
      setHasMore(res.meta?.has_more ?? false);
      setError(null);
    } catch {
      // A failed quiet refresh keeps the series on screen; the next tick tries again.
      if (live() && !quiet) setError('Failed to load history');
    } finally {
      if (live()) setLoading(false);
    }
  }, [unitId, period, type, limit, range?.from, range?.to, clock]);

  useEffect(() => {
    mounted.current = true;
    load();
    return () => { mounted.current = false; };
  }, [load]);
  useQuietRefresh(load, !!unitId && !isDemo(), HISTORY_REFRESH_MS[period]);

  return { records, loading, error, hasMore, reload: () => load() };
}

export interface UseHistoryAllResult {
  families: HistoryAll | null;
  loading: boolean;
  error: string | null;
}

/** type=all combined history — the three families in one fetch. */
export function useHistoryAll(
  unitId: string | null,
  period: HistoryPeriod,
  range?: HistoryRange,
): UseHistoryAllResult {
  const [families, setFamilies] = useState<HistoryAll | null>(null);
  const clock = useDeviceClock();
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const mounted = useRef(true);
  // Only the latest request may write: a slow answer for the previous point/period must not
  // land over the new one (Kimi #4). `mounted` alone is re-armed by the next effect.
  const gen = useRef(0);

  const load = useCallback(async (quiet = false) => {
    const my = ++gen.current;
    const live = () => mounted.current && gen.current === my;
    if (!unitId) { setFamilies(null); return; }
    if (isDemo()) {
      setFamilies({
        electrical: inWindow(demoHistory('electrical', period, 48, unitId) as HistoryElectricalRecord[], period, range, clock),
        energy: inWindow(demoHistory('energy', period, 48, unitId) as HistoryEnergyRecord[], period, range, clock),
        economics: inWindow(demoHistory('economics', period, 48, unitId) as HistoryEconomicsRecord[], period, range, clock),
      });
      return;
    }
    if (!quiet) { setLoading(true); setError(null); }
    try {
      const data = await getApiClient().getHistoryAll({
        unit_id: unitId, period,
        ...wireRange(range?.from, range?.to, period),
      });
      if (live()) setFamilies(data && {
        ...data,
        electrical: inWindow(data.electrical ?? [], period, range, clock),
        energy: inWindow(data.energy ?? [], period, range, clock),
        economics: inWindow(data.economics ?? [], period, range, clock),
      });
    } catch {
      if (live() && !quiet) setError('Failed to load history');
    } finally {
      if (live()) setLoading(false);
    }
  }, [unitId, period, range?.from, range?.to, clock]);

  useEffect(() => {
    mounted.current = true;
    load();
    return () => { mounted.current = false; };
  }, [load]);
  useQuietRefresh(load, !!unitId && !isDemo(), HISTORY_REFRESH_MS[period]);

  return { families, loading, error };
}
