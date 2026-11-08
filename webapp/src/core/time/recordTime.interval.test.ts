import { intervalStart, toIntervalStarts, wireRange } from './recordTime';

describe('history interval semantics (ts = END, canon ed876014)', () => {
  const mid = 1792076400; // 2026-10-16 00:00 JST (15:00Z)
  it('the hour closing at midnight starts at 23:00 of the day before', () => {
    expect(intervalStart({ ts: mid, dur: 3600 }, 'hour')).toBe(mid - 3600);
    expect(intervalStart({ ts: mid }, 'hour')).toBe(mid - 3600);
  });
  it('a reboot split keeps both parts in their hour', () => {
    expect(intervalStart({ ts: mid + 1020, dur: 1020 }, 'hour')).toBe(mid);
    expect(intervalStart({ ts: mid + 3600, dur: 2580 }, 'hour')).toBe(mid + 1020);
  });
  it('an over-long or late closing never spills into the neighbour', () => {
    expect(intervalStart({ ts: mid + 3603, dur: 4500 }, 'hour')).toBe(mid + 3);
  });
  it('pre-sync records keep their uptime ts', () => {
    expect(intervalStart({ ts: 5000, dur: 3600 }, 'hour')).toBe(5000);
  });
  it('rewrites once and keeps the wire ts', () => {
    const once = toIntervalStarts([{ ts: mid, dur: 3600 }], 'hour');
    expect(once[0]).toEqual({ ts: mid - 3600, dur: 3600, ts_end: mid });
    expect(toIntervalStarts(once, 'hour')).toEqual(once);
  });
  it('the wire query for a day asks for closings in (midnight, next midnight + 1 h]', () => {
    expect(wireRange(mid, mid + 86400, 'hour')).toEqual({ from: mid + 1, to: mid + 86400 + 3600 });
    expect(wireRange(undefined, undefined, 'day')).toEqual({});
  });
});

import { demoPosixTz } from '../../mock/demo';
import { parsePosixTz } from '../deviceTime';
describe('demo hub zone', () => {
  it('is the viewer zone as a valid POSIX TZ', () => {
    const tz = parsePosixTz(demoPosixTz());
    expect(tz?.stdOffsetSec).toBe(-new Date().getTimezoneOffset() * 60);
  });
});

import { makeDeviceClock } from '../deviceTime';

/** Kimi #3: calendar buckets start on the hub's calendar, not at ts − a fixed length. */
describe('calendar buckets start on the hub calendar', () => {
  const berlin = makeDeviceClock('CET-1CEST,M3.5.0,M10.5.0/3');
  const at = (iso: string) => Date.parse(iso) / 1000;
  it.each([
    ['January (31 d)', '2026-01-01T00:00:00+01:00', '2026-02-01T00:00:00+01:00'],
    ['February (28 d)', '2026-02-01T00:00:00+01:00', '2026-03-01T00:00:00+01:00'],
    ['February 2028 (29 d)', '2028-02-01T00:00:00+01:00', '2028-03-01T00:00:00+01:00'],
    ['March, clocks forward', '2026-03-01T00:00:00+01:00', '2026-04-01T00:00:00+02:00'],
    ['April (30 d)', '2026-04-01T00:00:00+02:00', '2026-05-01T00:00:00+02:00'],
  ])('month: %s', (_n, from, to) => {
    const dur = at(to) - at(from);
    expect(intervalStart({ ts: at(to), dur }, 'month', berlin)).toBe(at(from));
    expect(intervalStart({ ts: at(to) }, 'month', berlin)).toBe(at(from)); // no dur
    expect(intervalStart({ ts: at(to) + 4, dur }, 'month', berlin)).toBe(at(from)); // closed late
  });
  it('a 31-day month starts on the 1st, so a year filter from 1 January keeps it', () => {
    const [r] = toIntervalStarts([{ ts: at('2026-02-01T00:00:00+01:00'), dur: 31 * 86400 }], 'month', berlin);
    expect(r.ts).toBe(at('2026-01-01T00:00:00+01:00'));
  });
  it.each([
    ['23 h (clocks forward)', '2026-03-29T00:00:00+01:00', '2026-03-30T00:00:00+02:00'],
    ['25 h (clocks back)', '2026-10-25T00:00:00+02:00', '2026-10-26T00:00:00+01:00'],
    ['24 h', '2026-06-10T00:00:00+02:00', '2026-06-11T00:00:00+02:00'],
  ])('day: %s', (_n, from, to) => {
    expect(intervalStart({ ts: at(to), dur: at(to) - at(from) }, 'day', berlin)).toBe(at(from));
  });
  it('a partial day (restart at 15:00) still belongs to its day', () => {
    const end = at('2026-06-11T00:00:00+02:00');
    expect(intervalStart({ ts: end, dur: 9 * 3600 }, 'day', berlin)).toBe(at('2026-06-10T00:00:00+02:00'));
  });
  it('year: a leap year starts on 1 January', () => {
    expect(intervalStart({ ts: at('2029-01-01T00:00:00+01:00'), dur: 366 * 86400 }, 'year', berlin)).toBe(at('2028-01-01T00:00:00+01:00'));
  });
});
