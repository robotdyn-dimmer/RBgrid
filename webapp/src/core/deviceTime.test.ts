import { parsePosixTz, makeDeviceClock, tzOffsetAt } from './deviceTime';

describe('device time (POSIX TZ)', () => {
  it('parses fixed offsets (west-positive in POSIX)', () => {
    expect(parsePosixTz('<UTC+9>-9')?.stdOffsetSec).toBe(9 * 3600);
    expect(parsePosixTz('UTC0')?.stdOffsetSec).toBe(0);
    expect(parsePosixTz('UTC+0')?.stdOffsetSec).toBe(0);
    expect(parsePosixTz('MSK-3')?.stdOffsetSec).toBe(3 * 3600);
    expect(parsePosixTz('<UTC+9>')).toBeNull(); // no offset — invalid
    expect(parsePosixTz('')).toBeNull();
  });

  it('applies EU daylight saving rules', () => {
    const tz = parsePosixTz('CET-1CEST,M3.5.0,M10.5.0/3')!;
    expect(tzOffsetAt(tz, Date.UTC(2026, 0, 15) / 1000)).toBe(3600);      // January: CET
    expect(tzOffsetAt(tz, Date.UTC(2026, 6, 15) / 1000)).toBe(7200);      // July: CEST
  });

  it('device-local hour and midnight', () => {
    const c = makeDeviceClock('<UTC+9>-9');
    const ts = Date.UTC(2026, 9, 15, 20, 30) / 1000; // 20:30 UTC = 05:30 next day at UTC+9
    expect(c.parts(ts)).toMatchObject({ d: 16, h: 5, mi: 30 });
    expect(c.dayStart(ts)).toBe(Date.UTC(2026, 9, 15, 15, 0) / 1000); // 00:00 UTC+9
  });
});
