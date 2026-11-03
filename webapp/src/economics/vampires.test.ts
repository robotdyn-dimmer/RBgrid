import { standbyFloor, weekAverageRate, monthlyKwh, isNotableRise } from './vampires';
import { makeDeviceClock } from '../core/deviceTime';
import type { HistoryRecord } from '../types/history';
import type { TariffZone } from '../types/tariff-plan';

const clock = makeDeviceClock('UTC0');
const D0 = Date.UTC(2026, 9, 1) / 1000;
const hr = (day: number, h: number, p: number) => ({ ts: D0 + day * 86400 + h * 3600, dur: 3600, p_act: p }) as unknown as HistoryRecord;
const night = (day: number, p: number) => [1, 2, 3, 4].map((h) => hr(day, h, p));

describe('home vampires — standby floor', () => {
  it('needs 3 nights; one random dip does not set the floor', () => {
    expect(standbyFloor([...night(0, 37), ...night(1, 37)], clock)).toMatchObject({ w: null, nights: 2 });
    const hours = [...night(0, 37), ...night(1, 38), ...night(2, 36), hr(2, 12, 0)];
    expect(standbyFloor(hours, clock).w).toBe(37);
  });
  it('a night wash is atypical and skipped', () => {
    const hours = [...night(0, 37), ...night(1, 37), ...night(2, 2000), ...night(3, 37)];
    expect(standbyFloor(hours, clock)).toMatchObject({ w: 37, nights: 3 });
  });
  it('a new vampire shows as a trend', () => {
    const hours = [...night(0, 37), ...night(1, 37), ...night(2, 37), ...night(3, 41), ...night(4, 41), ...night(5, 41)];
    const f = standbyFloor(hours, clock);
    expect(f.trendW).toBe(4);
    expect(isNotableRise(f.trendW, 39)).toBe(true);
  });
  it('a night with export (the input does not show the house) is skipped', () => {
    const hours = [...night(0, -100), ...night(1, 37), ...night(2, 37), ...night(3, 37)];
    expect(standbyFloor(hours, clock)).toMatchObject({ w: 37, nights: 3 });
  });
  it('price: a week-weighted rate; 37 W ≈ 27 kWh a month', () => {
    const z = (id: string, sh: number, eh: number, rate: number) => ({ zone_id: id, zone_name: '', rate_buying_per_kwh: rate, is_enabled: true,
      conditions: [{ condition_type: 'TIME_RANGE', is_active: true, priority: 1, params: { start_hour: sh, start_minute: 0, end_hour: eh, end_minute: 0 } }] }) as unknown as TariffZone;
    expect(weekAverageRate([z('DAY', 7, 23, 6), z('NIGHT', 23, 7, 3)])).toBeCloseTo((16 * 6 + 8 * 3) / 24, 6);
    expect(monthlyKwh(37)).toBeCloseTo(27.01, 2);
  });
});
