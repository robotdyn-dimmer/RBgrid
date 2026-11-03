/**
 * «К вчера (к этому часу)» — ONE rule for the Overview and the History (rerun 16.10 HOME-R03 /
 * RES-R03: −62 %, −41 % and −4 % in the same minute). Today's CLOSED hours (from the hub's
 * midnight) against yesterday's hours over the same span; no projection, no mix of a live
 * counter with records. null when either side has no data or yesterday's span is incomplete.
 */
import { useMemo } from 'react';
import { useHistory } from './useHistory';
import { useDeviceClock } from '../core/deviceTime';
import { windowTotals } from '../pages/historyWindow';

const DAY = 86400;

export function useVsYesterday(unitId: string | null, money: boolean): { pct: number; today: number; yesterday: number } | null {
  const clock = useDeviceClock();
  const range = useMemo(() => {
    const now = Math.floor(Date.now() / 60_000) * 60;
    const today = clock.dayStart(now);
    return { from: clock.dayStart(today - DAY / 2), to: now + 3600 };
  }, [clock]);
  const hist = useHistory(unitId, 'hour', money ? 'economics' : 'energy', 60, range);
  return useMemo(() => {
    const today0 = clock.dayStart(Math.floor(Date.now() / 1000));
    const todayRecs = hist.records.filter((r) => r.ts >= today0);
    if (!todayRecs.length) return null;
    // The span today's closed hours cover: midnight → the end of the last closed hour.
    const lastEnd = Math.max(...todayRecs.map((r) => (r.ts_end ?? r.ts + 3600)));
    const cur = windowTotals(hist.records, { from: today0, to: lastEnd });
    const prev = windowTotals(hist.records, { from: today0 - DAY, to: lastEnd - DAY });
    if (prev.n < cur.n) return null; // yesterday has gaps over this span — no fair comparison
    const a = money ? cur.cost : cur.inWh;
    const b = money ? prev.cost : prev.inWh;
    if (!(b > 0)) return null;
    return { pct: ((a - b) / b) * 100, today: a, yesterday: b };
  }, [hist.records, clock, money]);
}
