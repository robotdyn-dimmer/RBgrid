import { bucketNominal, isPartial, qualityWord } from './HistoryPage';
import type { HistoryRecord } from '../types/history';

const rec = (o: Record<string, number>): HistoryRecord => ({ ts: 1792051200, ...o } as unknown as HistoryRecord);

describe('history quality (UX audit RES-01/17)', () => {
  it('nominal comes from the period, not from the data', () => {
    expect(bucketNominal(1792051200, 'hour')).toBe(3600);
    expect(bucketNominal(1792051200, 'day')).toBe(86400);
    // October 2026 → 31 days
    expect(bucketNominal(1792051200, 'month')).toBe(31 * 86400);
  });

  it('a complete hour is «полный» even next to an over-long one', () => {
    expect(isPartial(rec({ dur: 3600, q: 0 }), 'hour')).toBe(false);
    expect(qualityWord(rec({ dur: 3600, q: 0 }), 'hour')).toBe('полный');
    expect(qualityWord(rec({ dur: 4500, q: 0 }), 'hour')).toMatch(/длиннее нормы: 1 ч 15 мин/);
  });

  it('partial coverage and bits in words, no raw bit names', () => {
    expect(qualityWord(rec({ dur: 1200, q: 1 }), 'hour')).toBe('неполный: 20 мин из 1 ч');
    expect(qualityWord(rec({ dur: 3600, q: 256 }), 'hour')).toBe('перенесено из старого формата');
    expect(qualityWord(rec({ dur: 3600, q: 2 }), 'hour')).toBe('время не синхронизировано');
  });

  it('v3 records without dur: q is not quality', () => {
    expect(qualityWord(rec({ q: 120 }), 'hour')).toBe('полный');
  });
});

import { niceTicks } from './HistoryPage';
describe('round axis ticks (RES-23)', () => {
  it('1-2-5 steps with zero', () => {
    expect(niceTicks(-0.97, 1.21)).toEqual([-1, 0, 1, 2]);
    expect(niceTicks(0, 1506.81)).toEqual([0, 500, 1000, 1500, 2000]);
    expect(niceTicks(218, 229)).toEqual([215, 220, 225, 230]);
  });
});
