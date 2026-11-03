import { reconcileDays } from './reconcileDays';
import type { HistoryRecord } from '../../types/history';

const D0 = 1792022400; // a local midnight (UTC clock in the test)
const dayStart = (ts: number) => ts - (ts % 86400);
const hour = (start: number, ec: number, c = 0) => ({ ts: start, dur: 3600, q: 0, ec, es: 0, c, r: 0 }) as unknown as HistoryRecord;

describe('day records checked against the hour archive (RES-R01)', () => {
  it('a day closed with 0 while its hours hold energy takes the hours', () => {
    const days = [{ ts: D0, dur: 86400, q: 0, ec: 0, es: 0, c: 0, r: 0 } as unknown as HistoryRecord];
    const hours = [hour(D0 + 3600, 800, 0.3), hour(D0 + 7200, 1000, 0.4)];
    const [d] = reconcileDays(days, hours, dayStart, D0 + 3 * 86400) as (HistoryRecord & { reconciled?: boolean; ec: number; c: number })[];
    expect(d.ec).toBe(1800);
    expect(d.c).toBeCloseTo(0.7);
    expect(d.reconciled).toBe(true);
  });
  it('a day split into fragments is replaced as a whole, never counted twice', () => {
    const days = [
      { ts: D0, dur: 10800, q: 1, ec: 0 } as unknown as HistoryRecord,
      { ts: D0 + 21600, dur: 10800, q: 1, ec: 0 } as unknown as HistoryRecord,
    ];
    const out = reconcileDays(days, [hour(D0 + 3600, 2000), hour(D0 + 7200, 3800)], dayStart, D0 + 3 * 86400) as (HistoryRecord & { ec: number })[];
    expect(out).toHaveLength(1);
    expect(out[0].ec).toBe(5800);
  });
  it('overlapping copies of a day (Σ dur > a day) are not summed — the fullest one stays', () => {
    const days = [
      { ts: D0, dur: 80000, q: 1, ec: 5000 } as unknown as HistoryRecord,
      { ts: D0, dur: 60000, q: 1, ec: 3000 } as unknown as HistoryRecord,
    ];
    const out = reconcileDays(days, [], dayStart, D0 + 3 * 86400) as (HistoryRecord & { ec: number })[];
    expect(out).toHaveLength(1);
    expect(out[0].ec).toBe(5000);
  });
  it('a sound day is left as the hub wrote it', () => {
    const days = [{ ts: D0, dur: 86400, q: 0, ec: 1810, c: 0.7 } as unknown as HistoryRecord];
    const out = reconcileDays(days, [hour(D0, 1800, 0.7)], dayStart, D0 + 3 * 86400);
    expect(out[0]).toBe(days[0]);
  });
  it('a closed day with hours but no day record is built from the hours; today is not', () => {
    const now = D0 + 86400 + 5000;
    const out = reconcileDays([], [hour(D0, 500), hour(D0 + 86400, 300)], dayStart, now) as (HistoryRecord & { ec: number })[];
    expect(out).toHaveLength(1);
    expect(out[0]).toMatchObject({ ts: D0, ts_end: D0 + 86400, ec: 500, reconciled: true });
  });
});
