import { phaseRest } from './EnergyFlowPhases';

describe('«By phase» flow — the rest per phase', () => {
  it('phase P minus its single-phase lines; unknown when a line on it has no data', () => {
    const lines = [{ id: 'a', name: 'Kitchen', phase: 1, w: 100 }, { id: 'b', name: 'Pump', phase: 4, w: 900 }, { id: 'c', name: 'Lamp', phase: 2, w: null }];
    expect(phaseRest({ ph: 1, p: 156 }, lines)).toBe(56);
    expect(phaseRest({ ph: 2, p: 14 }, lines)).toBeNull();
    expect(phaseRest({ ph: 3, p: 47 }, lines)).toBe(47);
    expect(phaseRest({ ph: 3, p: null }, lines)).toBeNull();
  });
  it('sources count with the one sign (P > 0 into the point): producing PV < 0, charging battery > 0', () => {
    const lines = [{ id: 'pv', name: 'PV', phase: 1, w: -800, kind: 'gen' as const }, { id: 'bat', name: 'Battery', phase: 1, w: 500, kind: 'storage' as const }];
    // input L1 = −800 + 500 + rest → rest = 100 W when the phase imports −200 W
    expect(phaseRest({ ph: 1, p: -200 }, lines)).toBe(100);
  });
});
