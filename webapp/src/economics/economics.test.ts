import { liveRate, moneyPerHour, moneyForWh } from './costRate';
import { zoneTimeline, minutesToZoneChange, cheapestRate } from './tariffPlan';
import { expensiveNow, baseLoadMonthly, monthForecast, billShares, vsYesterday } from './insights';
import type { TariffPlan } from '../types/tariff-plan';

const plan = {
  zones: [
    { zone_id: 'DAY', zone_name: 'День', rate_buying_per_kwh: 6.43, is_enabled: true,
      conditions: [{ condition_type: 'TIME_RANGE', is_active: true, priority: 1, params: { start_hour: 7, start_minute: 0, end_hour: 23, end_minute: 0 } }] },
    { zone_id: 'NIGHT', zone_name: 'Ночь', rate_buying_per_kwh: 3.21, is_enabled: true,
      conditions: [{ condition_type: 'TIME_RANGE', is_active: true, priority: 1, params: { start_hour: 23, start_minute: 0, end_hour: 7, end_minute: 0 } }] },
  ],
} as unknown as TariffPlan;

describe('live rate (device system.tariff, E1)', () => {
  it('treats 0 rates as «no active plan»', () => {
    expect(liveRate({ buy_rate: 0, sell_rate: 0, zone: 'NIGHT', currency: 'EUR' }).buy).toBeNull();
  });
  it('costs consumption at buy, credits export at sell', () => {
    const r = liveRate({ buy_rate: 6, sell_rate: 2, zone: 'DAY', currency: 'RUB', cost_rate_h: 12 });
    expect(moneyPerHour(2000, r)).toBeCloseTo(12);
    expect(moneyPerHour(-1000, r)).toBeCloseTo(-2);
    expect(moneyForWh(500, r)).toBeCloseTo(3);
  });
});

describe('tariff timeline', () => {
  const spans = zoneTimeline(plan);
  it('splits a midnight-wrapping zone and finds the cheapest', () => {
    expect(spans.map((s) => s.zoneId)).toEqual(['NIGHT', 'DAY', 'NIGHT']);
    expect(cheapestRate(spans)).toEqual({ rate: 3.21, name: 'Ночь' });
  });
  it('minutes to the next zone change, across midnight', () => {
    expect(minutesToZoneChange(spans, new Date(2026, 9, 13, 22, 30))).toBe(30);
    expect(minutesToZoneChange(spans, new Date(2026, 9, 13, 23, 30))).toBe(7 * 60 + 30);
  });
});

describe('insights', () => {
  const spans = zoneTimeline(plan);
  it('«сейчас дорого» only when notably above the cheapest zone', () => {
    expect(expensiveNow(6.43, spans)?.ratio).toBeCloseTo(2.003, 2);
    expect(expensiveNow(3.21, spans)).toBeNull();
  });
  it('base load: 180 W × 720 h × 5 ₽ ≈ 648 ₽/мес', () => {
    expect(baseLoadMonthly(180, 5)?.money).toBeCloseTo(648);
    expect(baseLoadMonthly(0, 5)).toBeNull();
  });
  it('month forecast extrapolates the month-to-date cost', () => {
    const f = monthForecast(1000, new Date(2026, 9, 11, 0, 0)); // 10 full days elapsed of 31
    expect(f?.forecast).toBeCloseTo(3100);
    expect(monthForecast(10, new Date(2026, 9, 1, 2, 0))).toBeNull();
  });
  it('the pace is the days WITH data when the hub started mid-month (H3P-01)', () => {
    const now = new Date(2026, 9, 16, 16, 48); // 15,7 calendar days
    const f = monthForecast(3.24, now, 1.5)!;
    expect(f.partial).toBe(true);
    expect(f.forecast).toBeCloseTo((3.24 / 1.5) * 31, 6); // ≈ 67, not ≈ 6,4
    expect(monthForecast(3.24, now, 15.6)?.partial).toBe(false); // data from the 1st: calendar pace
    expect(monthForecast(0.4, now, 0.5)).toBeNull(); // under a day of data: no forecast
  });
  it('bill shares sum to 100 %: measured points past the top apart from the unmetered rest', () => {
    const s = billShares([{ name: 'Бойлер', wh: 5000 }, { name: 'Кухня', wh: 3000 }, { name: 'Свет', wh: null }], 10000, 60);
    expect(s.map((x) => x.name)).toEqual(['Бойлер', 'Кухня', 'Не измеряется']);
    const m = billShares([{ name: 'A', wh: 4000 }, { name: 'B', wh: 2000 }, { name: 'C', wh: 1000 }, { name: 'D', wh: 1000 }], 10000, 60, 2);
    expect(m.map((x) => [x.name, Math.round(x.share * 100)])).toEqual([['A', 40], ['B', 20], ['Остальные точки', 20], ['Не измеряется', 20]]);
    expect(s.reduce((a, x) => a + x.share, 0)).toBeCloseTo(1);
    expect(s[0].money).toBeCloseTo(30);
  });
  it('bill shares never exceed 100 % when lines out-sum the input', () => {
    const s2 = billShares([{ name: 'ЗС-1', wh: 18400 }, { name: 'Кухня', wh: 3200 }], 14200, 90);
    expect(s2.reduce((a, x) => a + x.share, 0)).toBeCloseTo(1);
    expect(s2.every((x) => x.share <= 1)).toBe(true);
  });
  it('vs yesterday ignores implausible jumps', () => {
    expect(vsYesterday(90, 4, new Date(2026, 9, 13, 12, 0))).toBeNull();
  });
  it('vs yesterday uses the projected full day', () => {
    const v = vsYesterday(50, 80, new Date(2026, 9, 13, 12, 0));
    expect(v?.projected).toBeCloseTo(100);
    expect(v?.pct).toBeCloseTo(25);
  });
});

import { minutesUntilRate } from './tariffPlan';
describe('time to the cheap zone (TAR-R04)', () => {
  const spans = [
    { zoneId: 'NIGHT', name: 'Ночь', rate: 3, from: 0, to: 420 },
    { zoneId: 'DAY', name: 'День', rate: 6, from: 420, to: 960 },
    { zoneId: 'EVENING_PEAK', name: 'Пик', rate: 9, from: 960, to: 1260 },
    { zoneId: 'DAY', name: 'День', rate: 6, from: 1260, to: 1380 },
    { zoneId: 'NIGHT', name: 'Ночь', rate: 3, from: 1380, to: 1440 },
  ];
  it('counts to the night, not to the evening peak', () => {
    expect(minutesUntilRate(spans, { h: 8, mi: 22 }, 3)).toBe(1380 - 502);
    expect(minutesUntilRate(spans, { h: 2, mi: 0 }, 3)).toBe(0);
  });
});

describe('live rate on the day-cost basis (3φ audit L3P-02)', () => {
  // eslint-disable-next-line @typescript-eslint/no-var-requires
  const { liveRate, grossOf } = require('./costRate');
  const plan = { vat_percent: 21, energy_tax_per_kwh: 0.0025, grid_usage_fee_per_kwh: 0.015, renewable_levy_per_kwh: 0.0065 };
  it('(rate + levies) × (1 + VAT) — .110: 0.30 → 0.392', () => {
    expect(grossOf(plan)(0.3)).toBeCloseTo(0.39204, 5);
    expect(grossOf({})).toBeNull();
  });
  it('buy rate and €/h scale together; sell stays as stated', () => {
    const r = liveRate({ zone: 'DAY', buy_rate: 0.3, sell_rate: 0.12, cost_rate_h: 0.0668, currency: 'EUR' }, plan);
    expect(r.buy).toBeCloseTo(0.39204, 5);
    expect(r.netPerHour).toBeCloseTo(0.0668 * (0.39204 / 0.3), 6);
    expect(r.sell).toBe(0.12);
    expect(r.gross).toBe(true);
    expect(liveRate({ zone: 'DAY', buy_rate: 0.3, cost_rate_h: 0.0668 }).buy).toBe(0.3);
  });
  it('the hub buy_rate_all_in wins, and its cost_rate_h is already on that basis', () => {
    const r = liveRate({ zone: 'DAY', buy_rate: 0.3, buy_rate_all_in: 0.392, cost_rate_h: 0.0873 }, plan);
    expect(r.buy).toBe(0.392);
    expect(r.netPerHour).toBe(0.0873);
    expect(r.gross).toBe(true);
  });
});

describe('monthForecast on the hub calendar (Kimi #16)', () => {
  it('uses the hub month, not the browser one', () => {
    // Browser still on 31 Oct 23:30; the hub is on 1 Nov 06:30 → a November forecast (30 days).
    const f = monthForecast(1, { y: 2026, mo: 11, d: 2, h: 0, mi: 0 })!;
    expect(f.daysInMonth).toBe(30);
    expect(f.elapsedDays).toBe(1);
    expect(monthForecast(1, { y: 2028, mo: 2, d: 11, h: 0, mi: 0 })?.daysInMonth).toBe(29);
  });
});
