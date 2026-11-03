import { parseGrid, deriveGridKind, phaseChoices, legsOfPhase, nominalFor, voltageBand, conductorsOf, PHASE_L1L2, PHASE_3P, PHASE_L3L1 } from './gridSystem';
import { normalizeTelemetryFrame, normalizePhases, unitMetricValue } from './model';
import { signedPowerW, fullPower, fullPowerW } from './siteModel';
import { buildFlowFrame } from '../components/flow/flowFrame';
import { buildUnitsJoin } from '../hooks/useUnitsConfig';
import { phaseLoads, measuredPhaseLoads, phaseImbalancePct, unmeteredW, gridPhaseVoltages } from '../pages/overviewData';
import { demoTelemetryFrame } from '../mock/demo';
import { mockConfig, mockSchema } from '../core/mocks';
import type { JsonObject } from '../core/json';

describe('gridSystem — mains system model', () => {
  it('reads a declared system and derives line-to-line', () => {
    const g = parseGrid({ system: 'SPLIT', nominal_v: 120, nominal_hz: 60 });
    expect(g).toMatchObject({ kind: 'SPLIT', nominalV: 120, lineV: 240, nominalHz: 60, legs: [1, 2], fromDevice: true });
    const t = parseGrid({ system: '3p', nominal_v: 230 });
    expect(t).toMatchObject({ kind: '3P', lineV: 400, legs: [1, 2, 3] }); // 400 as installers write it (IEC 60038), not √3 × 230 = 398
  });

  it('derives the system from phases when the hub does not declare it', () => {
    expect(deriveGridKind({ phases: [1, 1] })).toBe('1P');
    expect(deriveGridKind({ phases: [1, 2, 3] })).toBe('3P');
    expect(deriveGridKind({ inputPhase: PHASE_3P })).toBe('3P');
    expect(deriveGridKind({ phases: [1, PHASE_L1L2] })).toBe('SPLIT');
    // …but an UNSET system is 1P, as the hub counts it (root F20) — the phases only suggest.
    const g = parseGrid(undefined, { inputPhase: PHASE_3P, hardwareV: 230, hardwareHz: 50 });
    expect(g).toMatchObject({ kind: '1P', nominalV: 230, fromDevice: false, legs: [1] });
    expect(parseGrid(null).nominalV).toBe(230);
    // "" = not set → derived; hardware-config network nominals win over a rated_voltage_v guess.
    expect(parseGrid({ system: '' }, { phases: [1], hardwareV: 120, hardwareHz: 60, inputRatedV: 230 }))
      .toMatchObject({ kind: '1P', nominalV: 120, nominalHz: 60, fromDevice: false });
    // A 240 V input rating on a DECLARED split service is leg-to-leg → 120 V per leg.
    expect(parseGrid({ system: 'SPLIT' }, { inputRatedV: 240 }).nominalV).toBe(120);
  });

  it('offers the phase codes of the system and maps them onto legs', () => {
    expect(phaseChoices('SPLIT')).toEqual([0, 1, 2, PHASE_L1L2]);
    expect(phaseChoices('3P')).toEqual([0, 1, 2, 3, 4, 5, 6, 7]); // root delta №4
    expect(phaseChoices('1P')).toEqual([0, 1]);
    expect(legsOfPhase(PHASE_L3L1, parseGrid({ system: '3P' }))).toEqual([3, 1]);
    expect(deriveGridKind({ phases: [PHASE_L3L1] })).toBe('3P');
    const split = parseGrid({ system: 'SPLIT' });
    expect(legsOfPhase(PHASE_L1L2, split)).toEqual([1, 2]);
    expect(legsOfPhase(3, split)).toEqual([]); // no L3 on a split service
    expect(legsOfPhase(PHASE_3P, parseGrid({ system: '3P' }))).toEqual([1, 2, 3]);
  });
});

describe('phaseLoads on the US split-phase demo', () => {
  // The demo's powers follow its hourly model (W3-5) — pin an evening: ЗС-1 charging, the kitchen
  // on, the sun almost down (still exporting a little).
  beforeEach(() => { localStorage.setItem('rbgrid.demo.region', 'us'); jest.useFakeTimers().setSystemTime(new Date(2026, 9, 14, 19, 30)); });
  afterEach(() => { localStorage.removeItem('rbgrid.demo.region'); jest.useRealTimers(); });

  it('aggregates two legs; a two-pole single-CT line puts its measured P and I on each leg', () => {
    const data = normalizeTelemetryFrame(demoTelemetryFrame());
    const join = buildUnitsJoin((mockConfig('units').units ?? []) as JsonObject[]);
    const grid = parseGrid(mockConfig('system').grid, { phases: join.phases });
    expect(grid.kind).toBe('SPLIT');
    // Without the input's per-leg metering (ph[] absent this frame) → the estimate from lines.
    data.units.find((u) => u.unitKey === '9001')!.phases = undefined;
    const legs = phaseLoads(data.units, join, grid)!;
    expect(legs.map((l) => l.phase)).toEqual([1, 2]);
    // Leg L2 = EV-1 (L1L2, full current) − solar (L1L2, exporting, full current); EV-2 idles.
    const amps = (k: string) => Math.abs(unitMetricValue(data.units.find((u) => u.unitKey === k)!, 'current') ?? 0);
    expect(legs[1].currentA).toBeCloseTo(amps('9030') - amps('9021'), 6);
    const watts = (k: string) => signedPowerW(data.units.find((u) => u.unitKey === k)!) ?? 0;
    expect(legs[1].powerW).toBeCloseTo(Math.max(0, watts('9030') + watts('9021') + watts('9031')), 0);
    expect(legs[0].powerW).toBeGreaterThan(legs[1].powerW); // the kitchen is on L1
    expect(mockSchema().money?.currency_code).toBe('USD');
  });

  it('prefers the input\'s measured legs (U3I3 ph[]): U, I, P, cos φ; an omitted field stays absent', () => {
    const data = normalizeTelemetryFrame(demoTelemetryFrame());
    const join = buildUnitsJoin((mockConfig('units').units ?? []) as JsonObject[]);
    const grid = parseGrid(mockConfig('system').grid, { phases: join.phases });
    const legs = phaseLoads(data.units, join, grid)!;
    expect(legs.map((l) => [l.phase, l.measured])).toEqual([[1, true], [2, true]]);
    expect(legs[0].voltageV).toBeGreaterThan(110);
    expect(legs[0].pf).toBe(0.97);
    expect(legs[1].pf).toBeUndefined(); // L2 reports no pf
    expect(legs[0].powerW + legs[1].powerW).toBeGreaterThan(3000);
    expect(phaseImbalancePct(legs)).toBeGreaterThan(0);
  });
});

describe('measured input phases (U3I3 ph[], root delta №4)', () => {
  const join = buildUnitsJoin([{ unit_id: '9001', role: 'MAIN_SUPPLY', phase: 4 }] as unknown as JsonObject[]);
  const frame = (ph: unknown) => normalizeTelemetryFrame({
    v: 1, ts: 0, freq: 50, net_pwr: 0, net_dir: 0,
    units: { '9001': { src: 'i2c', present: true, i: 0, p_act: 0, ph } },
  } as never);

  it('normalizes ph[]: phase 1..3 only, no duplicates, sorted, unknown fields absent', () => {
    expect(normalizePhases([{ ph: 3, i: 1 }, { ph: 1, v: 230, i: 2, p: 400 }, { ph: 9, i: 1 }, { ph: 1, i: 5 }, null, { i: 3 }]))
      .toEqual([{ ph: 1, v: 230, i: 2, p: 400 }, { ph: 3, i: 1 }]);
    expect(normalizePhases([])).toBeUndefined();
    expect(normalizePhases(undefined)).toBeUndefined();
  });

  it('keeps the phase angle and the rotation (root 9bc08b40, live .110)', () => {
    expect(normalizePhases([{ ph: 2, v: 230.66, angle: -119.8 }, { ph: 1, v: 230.14, angle: 0 }]))
      .toEqual([{ ph: 1, v: 230.14, angle: 0 }, { ph: 2, v: 230.66, angle: -119.8 }]);
    const f = normalizeTelemetryFrame({ ts: 1, units: { '0006': { src: 'i2c', present: true, p_act: 30, phase_seq: 'reverse', ph: [{ ph: 1, v: 230 }] }, '0007': { src: 'i2c', present: true, phase_seq: 'sideways' } } } as never);
    expect(f.units.find((u) => u.unitKey === '0006')?.phaseSeq).toBe('reverse');
    expect(f.units.find((u) => u.unitKey === '0007')?.phaseSeq).toBeUndefined();
  });

  it('a 2-of-3 unit on a 3P site: the unreported phase has no data; unbalance from measured currents', () => {
    const g = frame([{ ph: 1, v: 230.1, i: 2.5, p: 560, q: 120, pf: 0.92 }, { ph: 3, v: 231, i: 1.5 }]);
    const rows = measuredPhaseLoads(g.units, join, parseGrid({ system: '3P' }))!;
    expect(rows.map((r) => [r.phase, r.hasData])).toEqual([[1, true], [2, false], [3, true]]);
    expect(rows[2].noPower).toBe(true); // ph3 carries no p
    expect(phaseImbalancePct(rows)).toBeCloseTo(25, 6); // mean 2 A, max deviation 0.5 A
  });

  it('no ph[] this frame → null (normal), and phaseLoads falls back to the estimate', () => {
    const g = frame(undefined);
    expect(measuredPhaseLoads(g.units, join, parseGrid({ system: '3P' }))).toBeNull();
    expect(phaseImbalancePct(phaseLoads(g.units, join))).toBeNull();
  });
});

describe('voltage band (nominal ±10 %, root: thresholds from hardware.network)', () => {
  it('flags outside ±10 % of the nominal; a two-pole line is checked against line-to-line', () => {
    expect(voltageBand(230, 230)).toBeNull();
    expect(voltageBand(206, 230)).toBe('low');
    expect(voltageBand(254, 230)).toBe('high');
    expect(voltageBand(120, 230)).toBe('low');
    expect(voltageBand(120, 120)).toBeNull();
    expect(voltageBand(0, 120)).toBeNull(); // no supply / not measured
    const us = parseGrid({ system: 'SPLIT', nominal_v: 120 });
    expect(nominalFor(us, PHASE_L1L2)).toBe(240);
    expect(voltageBand(238, nominalFor(us, PHASE_L1L2))).toBeNull();
    expect(nominalFor(us, 1)).toBe(120);
  });
});

describe('full power of a single-CT multi-phase point (W9, «≈ при балансе»)', () => {
  const frame = (units: Record<string, unknown>) => normalizeTelemetryFrame({ v: 1, ts: 0, freq: 50, net_pwr: 0, net_dir: 0, units } as never);
  const join = buildUnitsJoin([
    { unit_id: '9001', role: 'MAIN_SUPPLY', phase: 4 },
    { unit_id: '9030', point: 'ev', role: 'EV_CHARGING', phase: 4 },
    { unit_id: '9031', point: 'dryer', role: 'ZONE_LOAD', phase: PHASE_L1L2 },
    { unit_id: '9020', point: 'k', role: 'ZONE_LOAD', phase: 1 },
    { unit_id: '9040', point: 'grp', role: 'ZONE_LOAD', phase: 4, channel_idx: 254 },
  ] as unknown as JsonObject[]);
  const data = frame({
    '9001': { src: 'i2c', present: true, i: 10, p_act: 2000 },
    '9030': { src: 'i2c', present: true, point: 'ev', i: 10, p_act: 2300 },
    '9031': { src: 'i2c', present: true, point: 'dryer', i: 12, p_act: 1440 },
    '9020': { src: 'i2c', present: true, point: 'k', i: 2, p_act: 460 },
    '9040': { src: 'i2c', present: true, point: 'grp', i: 3, p_act: 2070 },
  });
  const g = (k: string) => data.units.find((u) => u.unitKey === k)!;

  it('3P → ×3, two-pole → ×2, single phase and phase groups as measured', () => {
    expect(conductorsOf(PHASE_3P)).toBe(3);
    expect(conductorsOf(PHASE_L3L1)).toBe(2);
    expect(conductorsOf(1)).toBe(1);
    expect(fullPower(g('9030'), join.forGroup(g('9030')))).toEqual({ w: 6900, estimated: true });
    expect(fullPower(g('9031'), join.forGroup(g('9031')))).toEqual({ w: 2880, estimated: true });
    expect(fullPower(g('9020'), join.forGroup(g('9020')))).toEqual({ w: 460, estimated: false });
    // channel_idx 254 = U3I3 phase group: top-level P is Σ already, even in a frame without ph[].
    expect(fullPower(g('9040'), join.forGroup(g('9040')))).toEqual({ w: 2070, estimated: false });
    // the input is also reachable by its legacy key
    expect(join.conductorsByUnit).toEqual({ '9001': 3, main: 3, '9030': 3, '9031': 2 });
  });

  it('the remainder, the flow house sum and the input use the full figures', () => {
    // input 2000 × 3 − (6900 + 2880 + 460 + 2070)
    expect(unmeteredW(data.units, join, fullPowerW(g('9001'), join.forGroup(g('9001'))))).toBe(6000 - 12310);
    const f = buildFlowFrame(data, { roleByUnit: join.roleByUnit, conductorsByUnit: join.conductorsByUnit });
    expect(f.grid).toBeCloseTo(6, 6);
    expect(f.loads.find((l) => l.id === '9030')!.v).toBeCloseTo(6.9, 6);
    // per-phase views keep the measured conductor
    expect(phaseLoads(data.units, join)![0].powerW).toBeCloseTo(2300 + 1440 + 460 + 2070, 0);
  });

  it('a unit reporting ph[] is Σ already → not multiplied', () => {
    const d = frame({ '9030': { src: 'i2c', present: true, point: 'ev', i: 10, p_act: 2300, ph: [{ ph: 1, i: 3, p: 800 }, { ph: 2, i: 3, p: 750 }, { ph: 3, i: 3, p: 750 }] } });
    expect(fullPower(d.units[0], join.forGroup(d.units[0]))).toEqual({ w: 2300, estimated: false });
  });
});

describe('live dir on rbAmp units (root canon 1cc8ffdd: 1 consuming · 2 supplying · 4 zero)', () => {
  it('signs power from dir, as injected on .139 (inj200)', () => {
    const d = normalizeTelemetryFrame({ v: 1, ts: 0, freq: 50, net_pwr: 0, net_dir: 0, units: {
      '9020': { src: 'i2c', present: true, p_act: 500, dir: 1 },
      '0100': { src: 'i2c', present: true, p_act: -300, dir: 2 },
      '9010': { src: 'i2c', present: true, p_act: 0, dir: 4 },
    } } as never);
    const w = (k: string) => signedPowerW(d.units.find((u) => u.unitKey === k)!);
    expect(w('9020')).toBe(500);
    expect(w('0100')).toBe(-300);
    expect(w('9010')).toBe(0);
  });
});

describe('flow with an absent input (.139 inj200: lines live, legacy main present:false)', () => {
  it('the house is Σ consumers (a supplying line subtracts), not the absent input\'s 0', () => {
    const d = normalizeTelemetryFrame({ v: 1, ts: 0, freq: 50, net_pwr: 0, net_dir: 0, units: {
      main: { src: 'i2c', present: false, p_act: 0, dir: 0 },
      '9020': { src: 'i2c', present: true, p_act: 500, dir: 1 },
      '0100': { src: 'i2c', present: true, p_act: -300, dir: 2 },
    } } as never);
    // configured roles as on .139 (DEVICE_LOAD) — without a role an exporting unit reads as generation
    const f = buildFlowFrame(d, { roleByUnit: { '9020': 'DEVICE_LOAD', '0100': 'DEVICE_LOAD' } as never });
    expect(f.house).toBeCloseTo(0.2, 6);
    expect(f.loads.find((l) => l.id === '0100')!.v).toBeCloseTo(-0.3, 6);
  });
});

describe('grid voltage per phase on the bus unit (canon fc16621e)', () => {
  const join = buildUnitsJoin([{ unit_id: '9001', role: 'MAIN_SUPPLY', phase: 4 }] as unknown as JsonObject[]);
  const frame = (bus: unknown) => normalizeTelemetryFrame({
    v: 1, ts: 0, freq: 50, net_pwr: 0, net_dir: 0,
    units: {
      '9001': { src: 'i2c', present: true, i: 3, p_act: 690, ph: [{ ph: 1, i: 1, p: 230 }, { ph: 2, i: 1, p: 230 }, { ph: 3, i: 1, p: 230 }] },
      v_bus: bus,
    },
  } as never);
  const vbus3 = {
    v: 230.3, present: true, src: 'i2c', src_class: 'rbamp', bus: 0, module: 2, ch: 255, n_ph: 3,
    ph: [{ ph: 1, v: 230.1, module: 2, vch: 0 }, { ph: 2, v: 231, module: 2, vch: 1 }, { ph: 3, v: 229.8, module: 2, vch: 2 }],
  };

  it('reads the bus ph[] as grid phase voltages; mean stays the top-level v', () => {
    const g = frame(vbus3);
    expect(gridPhaseVoltages(g.units)).toEqual([{ ph: 1, v: 230.1 }, { ph: 2, v: 231 }, { ph: 3, v: 229.8 }]);
    expect(unitMetricValue(g.units.find((u) => u.unitKey === 'v_bus')!, 'voltage')).toBeCloseTo(230.3, 6);
  });

  it('a current-only input leg takes its voltage from the bus phase', () => {
    const rows = measuredPhaseLoads(frame(vbus3).units, join, parseGrid({ system: '3P' }))!;
    expect(rows.map((r) => r.voltageV)).toEqual([230.1, 231, 229.8]);
  });

  it('no voltage module (v 0, absent) → no phases; single L1 → top-level v only', () => {
    expect(gridPhaseVoltages(frame({ v: 0, present: false, src: 'i2c', src_class: 'rbamp' }).units)).toEqual([]);
    expect(gridPhaseVoltages(frame({ v: 229, present: true, src: 'i2c', n_ph: 1 }).units)).toEqual([]);
  });
});

import { validatedKinds, phaseCodeValidated } from './gridSystem';
describe('hardware validation of mains systems (schema grid_system_validated)', () => {
  it('older hubs: only 1P; a phase code is validated when a validated system offers it', () => {
    expect(validatedKinds(undefined)).toEqual(['1P']);
    expect(validatedKinds(['1P', '3p'])).toEqual(['1P', '3P']);
    expect(phaseCodeValidated(1, ['1P'])).toBe(true);
    expect(phaseCodeValidated(PHASE_3P, ['1P'])).toBe(false);
    expect(phaseCodeValidated(PHASE_L1L2, ['1P', 'SPLIT'])).toBe(true);
  });
});
