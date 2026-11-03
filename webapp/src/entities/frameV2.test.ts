import { normalizeTelemetryFrame } from './model';
import { buildUnitsJoin } from '../hooks/useUnitsConfig';
import { mainUnit, lineUnits } from '../pages/overviewData';
import { buildFlowFrame } from '../components/flow/flowFrame';
import v2 from '../test/fixtures/fw-274a569c/telemetry-latest-v2.json';
import type { TelemetryFrame } from '../types/telemetry-units';

describe('telemetry frame v2 (root canon 274a569c, live .139)', () => {
  const data = normalizeTelemetryFrame(v2 as unknown as TelemetryFrame);
  const keys = data.units.map((u) => u.unitKey);
  const noConfig = buildUnitsJoin([]);

  it('maps the bus back to the pseudo key; the input keeps its hex', () => {
    expect(keys).toContain('v_bus');
    expect(keys).not.toContain('0007');
    expect(keys).toContain('0006');
    expect(data.units.find((u) => u.unitKey === '9010')!.role).toBe('DEVICE_LOAD');
  });

  it('classifies from the frame roles even before units-config is loaded', () => {
    expect(mainUnit(data.units, noConfig)?.unitKey).toBe('0006');
    expect(lineUnits(data.units, noConfig).map((u) => u.unitKey).sort()).toEqual(['0100', '9010', '9011', '9020']);
    const f = buildFlowFrame(data);
    expect(f.loads.map((l) => l.id).sort()).toEqual(['0100', '9010', '9011', '9020']);
  });

  it('an rbAmp input on v2 (role MAIN_SUPPLY, hex 9001) is found by its frame role', () => {
    const d = normalizeTelemetryFrame({ v: 2, ts: 0, freq: 50, net_pwr: 0, net_dir: 0, main_unit: '9001',
      units: { '9001': { present: true, p_act: 900, role: 'MAIN_SUPPLY', type: 'CURRENT_LOAD' }, '9020': { present: true, p_act: 300, role: 'DEVICE_LOAD' } } } as never);
    expect(mainUnit(d.units, noConfig)?.unitKey).toBe('9001');
    expect(lineUnits(d.units, noConfig).map((u) => u.unitKey)).toEqual(['9020']);
  });
});
