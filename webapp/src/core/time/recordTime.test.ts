import { isWallClockTs, isLegacyTs, isUnsyncedShelf, partitionByTime, WALLCLOCK_EPOCH_FLOOR } from './recordTime';

describe('record time semantics (block E.1 / spec §5.2)', () => {
  it('treats real epochs as wall-clock', () => {
    expect(isWallClockTs(1760140800)).toBe(true); // 2025-10
    expect(isWallClockTs(WALLCLOCK_EPOCH_FLOOR)).toBe(true);
  });

  it('treats uptime-style and ts=0 as legacy, never 1970', () => {
    expect(isWallClockTs(8360338)).toBe(false); // uptime seconds
    expect(isWallClockTs(0)).toBe(false);
    expect(isLegacyTs(8360338)).toBe(true);
    expect(isLegacyTs(null)).toBe(true);
    expect(isLegacyTs(undefined)).toBe(true);
  });

  it('identifies the ts=0 shelf', () => {
    expect(isUnsyncedShelf(0)).toBe(true);
    expect(isUnsyncedShelf(1760140800)).toBe(false);
  });

  it('partitions records into dated vs legacy preserving order', () => {
    const recs = [{ ts: 1760140800 }, { ts: 123 }, { ts: 1760144400 }, { ts: 0 }];
    const { dated, legacy } = partitionByTime(recs);
    expect(dated.map((r) => r.ts)).toEqual([1760140800, 1760144400]);
    expect(legacy.map((r) => r.ts)).toEqual([123, 0]);
  });
});
