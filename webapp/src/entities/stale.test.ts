import { normalizeTelemetryFrame, unitMetricValue } from './model';
import type { TelemetryFrame } from '../types/telemetry-units';

const frame = (u: Record<string, unknown>) => ({ v: 1, ts: 0, freq: 50, net_pwr: 0, net_dir: 0, units: { '9001': { present: true, p_act: 100, ...u } } }) as unknown as TelemetryFrame;

describe('held values (SCADA-R04: q_live STALE bit 4 / age_s)', () => {
  it('a STALE bit or age_s over 5 s marks the unit stale', () => {
    expect(normalizeTelemetryFrame(frame({ q_live: 4, age_s: 12 })).units[0]).toMatchObject({ stale: true, ageS: 12 });
    expect(normalizeTelemetryFrame(frame({ age_s: 30 })).units[0].stale).toBe(true);
  });
  it('a fresh sample or older firmware is not', () => {
    expect(normalizeTelemetryFrame(frame({ q_live: 0, age_s: 1 })).units[0].stale).toBe(false);
    expect(normalizeTelemetryFrame(frame({})).units[0].stale).toBe(false);
  });
  it('age_s null on a present unit = not read since boot: no numbers, no phases (root 16.10)', () => {
    const u = normalizeTelemetryFrame(frame({ age_s: null, p_act: 0, ph: [{ ph: 1, p: 0 }] })).units[0];
    expect(unitMetricValue(u, 'power')).toBeNull();
    expect(u.phases).toBeUndefined();
    expect(unitMetricValue(normalizeTelemetryFrame(frame({ age_s: 0.4 })).units[0], 'power')).toBe(100);
  });
});
