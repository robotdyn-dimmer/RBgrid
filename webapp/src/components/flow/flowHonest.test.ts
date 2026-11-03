import { normalizeTelemetryFrame, unitMetricValue, normalizePhases } from '../../entities/model';
import { buildFlowFrame, flowIdle } from './flowFrame';
import { gridValText, houseText } from './flowHome';

const frame = (units: Record<string, unknown>, extra: Record<string, unknown> = {}) => normalizeTelemetryFrame({
  v: 1, ts: 0, freq: 50, net_pwr: 0, net_dir: 0, units, ...extra,
} as never);

describe('honest «no measurement» (UX audit T3/T5)', () => {
  it('an absent unit has no live numbers; its energy totals stay', () => {
    const d = frame({ '9020': { src: 'i2c', present: false, v: 0, i: 0, p_act: 0, pf: 0, e_day_in: 120 } });
    const g = d.units[0];
    expect(unitMetricValue(g, 'power')).toBeNull();
    expect(unitMetricValue(g, 'voltage')).toBeNull();
    expect(unitMetricValue(g, 'energy_day_in')).toBe(120);
  });

  it('cos φ is hidden at no current and out of range', () => {
    const d = frame({ '9020': { src: 'i2c', present: true, v: 230, i: 0, p_act: 0, pf: -0.46 } });
    expect(unitMetricValue(d.units[0], 'power_factor')).toBeNull();
    expect(normalizePhases([{ ph: 1, i: 0, pf: 0.4 }, { ph: 2, i: 2, pf: 0.97 }, { ph: 3, i: 2, pf: -1.02 }]))
      .toEqual([{ ph: 1, i: 0 }, { ph: 2, i: 2, pf: 0.97 }, { ph: 3, i: 2 }]);
  });

  it('an absent input: grid «—», house unknown when nothing measures', () => {
    const d = frame({ '0006': { src: 'i2c', present: false, p_act: 0, dir: 4 }, '9020': { src: 'i2c', present: false, p_act: 0 } });
    const f = buildFlowFrame(d, { roleByUnit: { '0006': 'MAIN_SUPPLY', '9020': 'DEVICE_LOAD' } as never });
    expect(f.gridPresent).toBe(false);
    expect(gridValText(f)).toBe('—');
    expect(houseText(f)).toBe('—');
  });

  it('export with no generation/storage is flagged; no house load is invented', () => {
    const d = frame({ '0006': { src: 'i2c', present: true, p_act: -1062, dir: 2 }, '9020': { src: 'i2c', present: false, p_act: 0 } });
    const f = buildFlowFrame(d, { roleByUnit: { '0006': 'MAIN_SUPPLY', '9020': 'DEVICE_LOAD' } as never });
    expect(f.exportWithoutSource).toBe(true);
    expect(houseText(f)).toBe('—');
  });

  it('export WITH generation is normal', () => {
    const d = frame({ '0006': { src: 'i2c', present: true, p_act: -300, dir: 2 }, '9021': { src: 'i2c', present: true, p_act: -800 } });
    const f = buildFlowFrame(d, { roleByUnit: { '0006': 'MAIN_SUPPLY', '9021': 'RENEWABLE_SOURCE' } as never });
    expect(f.exportWithoutSource).toBe(false);
  });

  it('input only (no lines): «Прочее» stays at 0 W, so the schema does not vanish (.139 18.10)', () => {
    const roles = { '0006': 'MAIN_SUPPLY', '0007': 'MAIN_SUPPLY' } as never;
    const idle = buildFlowFrame(frame({ '0006': { src: 'i2c', present: true, v: 230, i: 0, p_act: 0 }, '0007': { src: 'i2c', present: true, v: 230 } }), { roleByUnit: roles });
    expect(idle.loads.map((l) => [l.id, l.v])).toEqual([['__other__', 0]]);
    const busy = buildFlowFrame(frame({ '0006': { src: 'i2c', present: true, v: 230, i: 2.3, p_act: 520 }, '0007': { src: 'i2c', present: true, v: 230 } }), { roleByUnit: roles });
    expect(busy.loads.map((l) => l.id)).toEqual(['__other__']);
    const absent = buildFlowFrame(frame({ '0006': { src: 'i2c', present: false, p_act: 0 } }), { roleByUnit: roles });
    expect(absent.loads).toEqual([]);
    expect(flowIdle(idle)).toBe(true);
    expect(flowIdle(busy)).toBe(false);
    expect(flowIdle(absent)).toBe(false); // «нет связи с вводом» is not «nothing flows»
    expect(flowIdle({ ...idle, lost: true })).toBe(false); // a lost frame freezes the last schema
  });
  it('a point on a channel the module lacks (UI3 in place of UI7) is not a load of the schema (.139 18.10)', () => {
    const f = frame({ '0006': { src: 'i2c', present: true, v: 230, i: 1.8, p_act: 430 }, '0101': { src: 'i2c', present: true, v: 230, i: 1.8, p_act: 430 },
      '0103': { src: 'i2c', present: false, p_act: 0 } });
    f.units = f.units.map((g) => (g.unitKey === '0103' ? { ...g, noChannel: true } : g));
    const ff = buildFlowFrame(f, { roleByUnit: { '0006': 'MAIN_SUPPLY' } as never });
    expect(ff.loads.map((l) => l.id)).toEqual(['0101']);
  });
});
