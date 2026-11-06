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

describe('southern-hemisphere DST presets (content dst-toggle-dead)', () => {
  const at = (tz: string, iso: string) => tzOffsetAt(parsePosixTz(tz)!, Date.parse(iso) / 1000) / 3600;
  it('summer time runs across New Year', () => {
    expect(at('AEST-10AEDT,M10.1.0,M4.1.0/3', '2026-01-15T00:00:00Z')).toBe(11);
    expect(at('AEST-10AEDT,M10.1.0,M4.1.0/3', '2026-07-15T00:00:00Z')).toBe(10);
    expect(at('ACST-9:30ACDT,M10.1.0,M4.1.0/3', '2026-01-15T00:00:00Z')).toBe(10.5);
    expect(at('NZST-12NZDT,M9.5.0,M4.1.0/3', '2026-01-15T00:00:00Z')).toBe(13);
    expect(at('NZST-12NZDT,M9.5.0,M4.1.0/3', '2026-07-15T00:00:00Z')).toBe(12);
    expect(at('<-04>4<-03>,M9.1.6/24,M4.1.6/24', '2026-01-15T00:00:00Z')).toBe(-3);
    expect(at('<-04>4<-03>,M9.1.6/24,M4.1.6/24', '2026-07-15T00:00:00Z')).toBe(-4);
  });
  it('Sydney changes over on the first Sunday of April at 03:00 summer time', () => {
    // 2026-04-05 03:00 AEDT = 2026-04-04T16:00Z
    expect(at('AEST-10AEDT,M10.1.0,M4.1.0/3', '2026-04-04T15:59:00Z')).toBe(11);
    expect(at('AEST-10AEDT,M10.1.0,M4.1.0/3', '2026-04-04T16:00:00Z')).toBe(10);
  });
});

describe('day start on a changeover day', () => {
  const ds = (tz: string, iso: string) => new Date(makeDeviceClock(tz).dayStart(Date.parse(iso) / 1000) * 1000).toISOString();
  it('midnight keeps its own offset', () => {
    // Berlin 29.03.2026: spring forward at 02:00; midnight was CET (+1)
    expect(ds('CET-1CEST,M3.5.0,M10.5.0/3', '2026-03-29T10:00:00Z')).toBe('2026-03-28T23:00:00.000Z');
    // Berlin 25.10.2026: fall back at 03:00; midnight was CEST (+2)
    expect(ds('CET-1CEST,M3.5.0,M10.5.0/3', '2026-10-25T12:00:00Z')).toBe('2026-10-24T22:00:00.000Z');
    // an ordinary day
    expect(ds('CET-1CEST,M3.5.0,M10.5.0/3', '2026-07-15T12:00:00Z')).toBe('2026-07-14T22:00:00.000Z');
  });
  it('Santiago spring-forward: no local midnight, the day starts at 01:00 −03 like the hub', () => {
    // first Saturday of September 2026 = 05.09, 24:00 −04 = 06.09 04:00Z
    expect(ds('<-04>4<-03>,M9.1.6/24,M4.1.6/24', '2026-09-06T15:00:00Z')).toBe('2026-09-06T04:00:00.000Z');
    // fall back: first Saturday of April 2026 = 04.04, 24:00 −03 → 23:00 −04; Sunday starts 05.04 04:00Z
    expect(ds('<-04>4<-03>,M9.1.6/24,M4.1.6/24', '2026-04-05T15:00:00Z')).toBe('2026-04-05T04:00:00.000Z');
  });
});
