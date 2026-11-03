import { windowSpan, windowSpanAt, windowTotals, deltaPct } from './historyWindow';
import { makeDeviceClock } from '../core/deviceTime';
import type { HistoryRecord } from '../types/history';

const clock = makeDeviceClock('<UTC+9>-9');
const now = Date.UTC(2026, 9, 16, 3, 30) / 1000; // 12:30 on 16 Oct at UTC+9
const local0 = (y: number, m: number, d: number) => Date.UTC(y, m - 1, d) / 1000 - 9 * 3600;

describe('history windows (W2-1)', () => {
  it('today / yesterday by the hub clock, hourly', () => {
    const t0 = windowSpan('today', now, clock);
    expect([t0.from, t0.to, t0.step]).toEqual([local0(2026, 10, 16), now, 'hour']);
    // Compared up to the start of the running hour — the hours today's side has closed (RES-R03).
    const hourStart = local0(2026, 10, 16) + Math.floor((now - local0(2026, 10, 16)) / 3600) * 3600;
    expect(t0.prev).toEqual({ from: local0(2026, 10, 15), to: hourStart - 86400 });
    const y = windowSpan('yesterday', now, clock);
    expect([y.from, y.to]).toEqual([local0(2026, 10, 15), local0(2026, 10, 16)]);
  });

  it('this / last month by days, last year to date by months', () => {
    const m = windowSpan('thisMonth', now, clock);
    expect([m.from, m.step]).toEqual([local0(2026, 10, 1), 'day']);
    expect(m.prev.from).toBe(local0(2026, 9, 1));
    const lm = windowSpan('lastMonth', now, clock);
    expect([lm.from, lm.to]).toEqual([local0(2026, 9, 1), local0(2026, 10, 1)]);
    expect(lm.prev).toEqual({ from: local0(2026, 8, 1), to: local0(2026, 9, 1) });
    const yr = windowSpan('year', now, clock);
    expect([yr.from, yr.step, yr.prev.from]).toEqual([local0(2026, 1, 1), 'month', local0(2025, 1, 1)]);
  });

  it('totals count records inside the window; delta vs the previous one', () => {
    const rec = (ts: number, o: Record<string, number>) => ({ ts, ...o } as unknown as HistoryRecord);
    const span = { from: 100, to: 200 };
    const tot = windowTotals([rec(90, { ec: 5 }), rec(100, { ec: 1000, es: 10, c: 0.3 }), rec(150, { ec: 500 }), rec(200, { ec: 7 })], span);
    expect(tot).toMatchObject({ inWh: 1500, outWh: 10, cost: 0.3, n: 2 });
    expect(deltaPct(1500, 1000)).toBe(50);
    expect(deltaPct(1500, 0)).toBeNull();
  });
});

describe('history window navigation (HA-R10)', () => {
  const clk = makeDeviceClock('UTC0');
  const now = Date.UTC(2026, 9, 16, 10, 30) / 1000;
  it('back = 1 on «this month» is the whole of September, compared to August', () => {
    const s = windowSpanAt('thisMonth', 1, now, clk);
    expect([s.from, s.to]).toEqual([Date.UTC(2026, 8, 1) / 1000, Date.UTC(2026, 9, 1) / 1000]);
    expect(s.prev).toEqual({ from: Date.UTC(2026, 7, 1) / 1000, to: Date.UTC(2026, 8, 1) / 1000 });
  });
  it('back = 2 on «today» is the whole day before yesterday', () => {
    const s = windowSpanAt('today', 2, now, clk);
    expect([s.from, s.to]).toEqual([Date.UTC(2026, 9, 14) / 1000, Date.UTC(2026, 9, 15) / 1000]);
  });
});
