import { modulePollFailSince } from './modulePollFail';
import { HubEvent } from '../../types/hubEvents';

const ev = (seq: number, ts: number, msg: string, lvl: HubEvent['lvl'] = 'event'): HubEvent => ({ seq, b: 1, ts, up: seq, lvl, msg });
const T = 1_792_000_000;

describe('modulePollFailSince', () => {
  it('takes the first FAIL of the failure still open', () => {
    const m = modulePollFailSince([
      ev(1, T, 'FAIL m2 poll: timeout (1 failed, 0s) — module unreachable', 'fail'),
      ev(2, T + 60, 'FAIL m2 poll: timeout (20 failed, 60s) — module unreachable', 'fail'),
    ]);
    expect(m.get(2)).toEqual({ ts: T, kind: 'poll' });
  });
  it('forgets a failure once restored, starts over on the next', () => {
    const m = modulePollFailSince([
      ev(1, T, 'FAIL m1 poll: nack (1 failed, 0s) — module unreachable', 'fail'),
      ev(2, T + 30, 'm1 poll restored after 30s (10 failed polls)'),
      ev(3, T + 500, 'FAIL m1 poll: nack (1 failed, 0s) — module unreachable', 'fail'),
      ev(4, T + 600, 'm0 poll restored after 5s (2 failed polls)'),
    ]);
    expect(m.get(1)?.ts).toBe(T + 500);
    expect(m.has(0)).toBe(false);
  });
  it('ignores other lines and lines without a wall clock', () => {
    const m = modulePollFailSince([
      ev(1, T, 'QTR m0 dt=1ms cnt=+0Wh'),
      ev(2, 0, 'FAIL m3 poll: timeout — module unreachable', 'fail'),
      ev(3, T, 'FAIL poll slow m0 120ms', 'fail'),
    ]);
    expect(m.size).toBe(0);
  });
  it('reads «not found at boot» until the module joins late', () => {
    const lines = [ev(1, T, 'FAIL m2 (0x52) not found at boot — healer re-probes every 10s', 'fail')];
    expect(modulePollFailSince(lines).get(2)).toEqual({ ts: T, kind: 'boot' });
    expect(modulePollFailSince([...lines, ev(2, T + 40, 'm2 (0x52) joined late — provisioned')]).has(2)).toBe(false);
  });
});
