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
