/**
 * useYesterdayHourly — the hub's YESTERDAY by the hour for a few points: hub-local hour (0–23) → Wh
 * imported, summed over the points (the EV charger's three phases are three points). Hour energy
 * records of the closed day; re-read when the day changes.
 */
import { useEffect, useState } from 'react';
import { getApiClient } from '../services/api';
import { isDemo } from '../services/connection';
import { demoHistory } from '../mock/demo';
import { toIntervalStarts, wireRange } from '../core/time/recordTime';
import { useDeviceClock } from '../core/deviceTime';
import type { HistoryEnergyRecord, HistoryElectricalRecord } from '../types/history';

export function useYesterdayHourly(hexIds: string[]): Map<number, number> | null {
  const clock = useDeviceClock();
  const [out, setOut] = useState<Map<number, number> | null>(null);
  const sig = hexIds.join(',');
  const today = clock.dayStart(Math.floor(Date.now() / 1000));
  useEffect(() => {
    let alive = true;
    const ids = sig ? sig.split(',') : [];
    if (!ids.length) { setOut(null); return undefined; }
    const from = clock.dayStart(today - 43_200);
    const load = async (id: string): Promise<HistoryEnergyRecord[]> => {
      const recs = isDemo()
        ? (demoHistory('energy', 'hour', 60, id) as HistoryEnergyRecord[])
        : ((await getApiClient().getHistory<HistoryEnergyRecord>({ unit_id: id, period: 'hour', type: 'energy', offset: 0, limit: 30, ...wireRange(from, today, 'hour') })).data ?? []);
      return toIntervalStarts(recs, 'hour').filter((r) => r.ts >= from && r.ts < today);
    };
    void Promise.allSettled(ids.map(load)).then((res) => {
      if (!alive) return;
      const m = new Map<number, number>();
      let any = false;
      res.forEach((r) => {
        if (r.status !== 'fulfilled') return;
        any = true;
        for (const rec of r.value) {
          const h = clock.parts(rec.ts).h;
          m.set(h, (m.get(h) ?? 0) + Math.max(0, rec.ec ?? 0));
        }
      });
      setOut(any ? m : null);
    });
    return () => { alive = false; };
  }, [sig, today, clock]);
  return out;
}

/**
 * The hourly mean power summed over a few points (hour electrical records in [from, to)):
 * interval start (s) → W, only hours every point has. Null until loaded or with no points.
 */
export function useHourPowerSum(hexIds: string[], from: number, to: number): Map<number, number> | null {
  const [out, setOut] = useState<Map<number, number> | null>(null);
  const sig = hexIds.join(',');
  useEffect(() => {
    let alive = true;
    const ids = sig ? sig.split(',') : [];
    if (!ids.length) { setOut(null); return undefined; }
    const load = async (id: string): Promise<HistoryElectricalRecord[]> => {
      const recs = isDemo()
        ? (demoHistory('electrical', 'hour', 200, id) as HistoryElectricalRecord[])
        : ((await getApiClient().getHistory<HistoryElectricalRecord>({ unit_id: id, period: 'hour', type: 'electrical', offset: 0, limit: 200, ...wireRange(from, to, 'hour') })).data ?? []);
      return toIntervalStarts(recs, 'hour').filter((r) => r.ts >= from && r.ts < to && typeof r.p_act === 'number');
    };
    void Promise.allSettled(ids.map(load)).then((res) => {
      if (!alive) return;
      if (res.some((r) => r.status !== 'fulfilled')) { setOut(null); return; }
      const sum = new Map<number, number>();
      const seen = new Map<number, number>();
      res.forEach((r) => {
        if (r.status !== 'fulfilled') return;
        for (const rec of r.value) {
          sum.set(rec.ts, (sum.get(rec.ts) ?? 0) + rec.p_act);
          seen.set(rec.ts, (seen.get(rec.ts) ?? 0) + 1);
        }
      });
      seen.forEach((n, ts) => { if (n < ids.length) sum.delete(ts); });
      setOut(sum);
    });
    return () => { alive = false; };
  }, [sig, from, to]);
  return out;
}
