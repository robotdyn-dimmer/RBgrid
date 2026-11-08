import { zoneCoverage, zoneRange, zoneDays, matchZone, daysText, WEEKEND_DAYS } from './zoneCoverage';
import type { TariffZone } from '../types/tariff-plan';

const z = (id: string, sh: number, eh: number, on = true, mask?: number): TariffZone => ({
  zone_id: id as TariffZone['zone_id'], zone_name: '', rate_buying_per_kwh: 1, is_enabled: on,
  conditions: [
    { condition_type: 'TIME_RANGE', is_active: true, priority: 1, params: { start_hour: sh, start_minute: 0, end_hour: eh, end_minute: 0, spans_midnight: eh <= sh } },
    ...(mask !== undefined ? [{ condition_type: 'WEEKDAY' as const, is_active: true, priority: 5, params: { weekday_mask: mask } }] : []),
  ],
});

describe('tariff zones over the week (W2-3, firmware rules)', () => {
  it('day + night across midnight cover every day exactly', () => {
    const c = zoneCoverage([z('DAY', 7, 23), z('NIGHT', 23, 7)]);
    expect(c.gaps).toEqual([]);
    expect(c.overlaps).toEqual([]);
  });
  it('a disabled zone leaves a gap on all 7 days; the default zone takes it', () => {
    const c = zoneCoverage([z('DAY', 7, 23), z('NIGHT', 23, 7, false)], 'DAY');
    expect(c.gaps).toEqual([{ start: 0, end: 420, days: 0x7f }, { start: 1380, end: 1440, days: 0x7f }]);
    expect(c.fallback).toBe(0);
    expect(c.week[0][60]).toBe(0);
  });
  it('an overlap is resolved by zone type, as on the hub (peak beats day)', () => {
    const zones = [z('DAY', 7, 23), z('EVENING_PEAK', 18, 21), z('NIGHT', 23, 7)];
    const c = zoneCoverage(zones);
    expect(c.overlaps).toEqual([{ start: 1080, end: 1260, days: 0x7f, winner: 1, losers: [0] }]);
    expect(matchZone(zones, 2, 19 * 60)).toBe(1);
    expect(c.shadowed).toEqual([]);
  });
  it('a weekday mask is Monday-origin; a weekend zone wins Sat/Sun all day', () => {
    const zones = [z('DAY', 7, 23), z('NIGHT', 23, 7), { zone_id: 'WEEKEND', zone_name: '', rate_buying_per_kwh: 1, is_enabled: true,
      conditions: [{ condition_type: 'WEEKEND', is_active: true, priority: 80, params: {} }] } as TariffZone];
    expect(zoneDays(zones[2])).toBe(WEEKEND_DAYS);
    expect(matchZone(zones, 5, 12 * 60)).toBe(2); // Saturday noon
    expect(matchZone(zones, 4, 12 * 60)).toBe(0); // Friday noon
    const peakWeekdays = z('EVENING_PEAK', 18, 21, true, 0x1f);
    expect(matchZone([z('DAY', 0, 0), peakWeekdays], 6, 19 * 60)).toBe(0); // Sunday — no peak
    expect(matchZone([z('DAY', 0, 0), peakWeekdays], 0, 19 * 60)).toBe(1); // Monday — peak
  });
  it('a zone that never wins is reported', () => {
    const c = zoneCoverage([z('NIGHT', 0, 0), z('DAY', 7, 23)]);
    expect(c.shadowed).toEqual([{ zone: 1, by: [0] }]);
  });
  it('OR logic: time OR days', () => {
    const or = { ...z('EVENING_PEAK', 18, 21, true, 0x60), conditions_and_logic: false };
    expect(matchZone([z('DAY', 0, 0), or], 0, 19 * 60)).toBe(1); // Mon evening — time matches
    expect(matchZone([z('DAY', 0, 0), or], 5, 10 * 60)).toBe(1); // Sat morning — day matches
    expect(matchZone([z('DAY', 0, 0), or], 0, 10 * 60)).toBe(0);
  });
  it('days in words', () => {
    const short = ['Пн', 'Вт', 'Ср', 'Чт', 'Пт', 'Сб', 'Вс'];
    expect(daysText(0x1f, short, 'каждый день')).toBe('Пн–Пт');
    expect(daysText(0x60, short, 'каждый день')).toBe('Сб, Вс');
    expect(daysText(0x7f, short, 'каждый день')).toBe('каждый день');
    expect(daysText(0x15, short, 'каждый день')).toBe('Пн, Ср, Пт');
  });
  it('zoneRange reads the time condition', () => {
    expect(zoneRange(z('NIGHT', 23, 7))).toEqual({ start: 1380, end: 420 });
  });
});

import { parseHhmm } from './zoneCoverage';

describe('parseHhmm — 24:00 is the only 24:mm (Kimi #20)', () => {
  it.each([['23:59', 1439], ['24:00', 1440], ['0:30', 30], ['24:01', null], ['24:30', null], ['25:00', null], ['12:60', null]])('%s', (s, v) => {
    expect(parseHhmm(s)).toBe(v);
  });
});

import { hitsAt } from './zoneCoverage';

describe('zone conditions one by one, as the hub (Kimi review)', () => {
  const tr = (sh: number, eh: number) => ({ condition_type: 'TIME_RANGE', is_active: true, priority: 10, params: { start_hour: sh, start_minute: 0, end_hour: eh, end_minute: 0, spans_midnight: false } });
  const wd = (mask: number) => ({ condition_type: 'WEEKDAY', is_active: true, priority: 10, params: { weekday_mask: mask } });
  const zone = (o: Record<string, unknown>) => ({ zone_id: 'EVENING_PEAK', is_enabled: true, ...o }) as never;
  it('two time ranges OR-ed: both windows are the zone', () => {
    const z = [zone({ conditions_and_logic: false, conditions: [tr(7, 9), tr(17, 20)] })];
    expect(hitsAt(z, 0, 8 * 60).win).toBe(0);
    expect(hitsAt(z, 0, 18 * 60).win).toBe(0);
    expect(hitsAt(z, 0, 12 * 60).win).toBe(-1);
  });
  it('two weekday masks OR-ed: either day set', () => {
    const z = [zone({ conditions_and_logic: false, conditions: [wd(0x01), wd(0x10)] })]; // Mon | Fri
    expect(hitsAt(z, 0, 600).win).toBe(0);
    expect(hitsAt(z, 4, 600).win).toBe(0);
    expect(hitsAt(z, 2, 600).win).toBe(-1);
  });
  it('AND: time × days', () => {
    const z = [zone({ conditions: [tr(17, 20), wd(0x1f)] })];
    expect(hitsAt(z, 1, 18 * 60).win).toBe(0);
    expect(hitsAt(z, 6, 18 * 60).win).toBe(-1);
  });
});
