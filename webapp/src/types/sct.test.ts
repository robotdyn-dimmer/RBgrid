import { sctUncorrected } from './sensors';
import { markModuleRestart, restartingUnitKeys } from '../hooks/useHardware';
import type { SensorListEntry } from './sensors';

describe('SCT correction (rbamp 1.7.2, root 20.10)', () => {
  it('flags every status but «corrected»; no field = nothing to say', () => {
    expect(sctUncorrected({ sct_correction: 'corrected' })).toBe(false);
    for (const s of ['uncalibrated', 'gain_unexpected', 'unknown_model']) expect(sctUncorrected({ sct_correction: s })).toBe(true);
    expect(sctUncorrected({})).toBe(false);
    expect(sctUncorrected(null)).toBe(false);
  });
});

describe('module restart after a CT model change', () => {
  const s = (unit_id: string, module: number) => ({ unit_id, module } as unknown as SensorListEntry);
  it('marks the points of that module only, for the grace window', () => {
    markModuleRestart(0, 5000);
    const now = Date.now();
    expect(Array.from(restartingUnitKeys([s("0006", 0), s("0101", 0), s("0201", 1)], now))).toEqual(['0006', '0101']);
    expect(restartingUnitKeys([s('0006', 0)], now + 6000).size).toBe(0);
  });
});
