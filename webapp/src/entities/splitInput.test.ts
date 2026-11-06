/** The input split per phase (fw 1.1.5) is read as one input — shapes from .139 06.11 (root u3i3-per-phase-points). */
import { foldSplitInput, splitInputLegs } from './splitInput';
import { normalizeTelemetryFrame } from './model';
import type { TelemetryFrame } from '../types/telemetry-units';

const frame = {
  v: 2, ts: 1, sntp: 1, freq: 50, net_dir: 4, net_pwr: 0, main_unit: '0006', vbus_unit: '0007',
  units: {
    '0007': { src: 'i2c', v: 225.7, present: true, module: 2, ch: 255, role: 'MAIN_SUPPLY' },
    '0006': { src: 'i2c', v: 225.6, i: 1.5, p_act: -320, present: true, module: 2, ch: 0, ch_label: 'L1', e_in: 0, e_out: 73.9, role: 'MAIN_SUPPLY' },
    '0102': { src: 'i2c', i: 2, p_act: 400, present: true, point: 'kitchen', module: 1, ch: 0, ch_label: 'I0', e_in: 241, e_out: 0, role: 'DEVICE_LOAD' },
    '0103': { src: 'i2c', v: 226, i: 2, p_act: 400, present: true, point: 'grid-l2', module: 2, ch: 1, ch_label: 'L2', e_in: 92.3, e_out: 0, role: 'MAIN_SUPPLY' },
    '0104': { src: 'i2c', v: 227, i: 2, p_act: 400, present: true, point: 'grid-l3', module: 2, ch: 2, ch_label: 'L3', e_in: 92.3, e_out: 0, role: 'MAIN_SUPPLY' },
  },
} as unknown as TelemetryFrame;

describe('input split per phase', () => {
  it('finds the other phase points, not the voltage bus or a line', () => {
    expect(splitInputLegs(frame)).toEqual(['0103', '0104']);
  });
  it('sums the legs into the input and lists them as its phases', () => {
    const f = foldSplitInput(frame);
    const m = f.units['0006'];
    expect(m.p_act).toBe(480);
    expect(m.e_in).toBeCloseTo(184.6, 6);
    expect(m.e_out).toBeCloseTo(73.9, 6);
    expect((m as { ph?: { ph: number; p?: number }[] }).ph?.map((r) => [r.ph, r.p])).toEqual([[1, -320], [2, 400], [3, 400]]);
    // the legs and the lines are untouched
    expect(f.units['0103'].p_act).toBe(400);
    expect(f.units['0102'].p_act).toBe(400);
    const g = normalizeTelemetryFrame(f).units.find((u) => u.unitKey === '0006');
    expect(g?.phases?.length).toBe(3);
  });
  it('leaves a grouped or single input alone', () => {
    const one = { ...frame, units: { '0006': frame.units['0006'], '0102': frame.units['0102'] } } as TelemetryFrame;
    expect(foldSplitInput(one)).toBe(one);
  });
});
