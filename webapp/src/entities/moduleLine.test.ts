import { isBasic, pointHardware, ctRatingA, isCurrentOnly } from './moduleLine';

const models = { 1: 'SCT-013-005', 4: 'SCT-013-050', 7: 'SCT-013-060' };
const sensors = [
  { iface: 'i2c' as const, present: true, unit_id: '0006', bus: 0, module: 0, ch: 0, ct_code: 4 },
  { iface: 'i2c' as const, present: true, unit_id: '0007', bus: 0, module: 0, ch: 255 },
];
const modules = [{ module: 0, present: true, line: 'basic', model: 'BASIC-UI1', variant: 1, n_v: 1, n_i: 1, fw: 11 }];

describe('BASIC module (live .139, root dd861a54)', () => {
  it('finds the module behind a point and knows it is BASIC', () => {
    const hw = pointHardware('0006', sensors, modules);
    expect(hw.sensor?.ct_code).toBe(4);
    expect(isBasic(hw.module)).toBe(true);
    expect(isBasic({ module: 1, present: true, line: 'std' })).toBe(false);
    expect(isCurrentOnly({ module: 2, present: true, n_v: 0, n_i: 2 })).toBe(true);
  });
  it('CT rating from the hub model name', () => {
    expect(ctRatingA(4, models)).toBe(50);
    expect(ctRatingA(9, models)).toBeNull();
    expect(ctRatingA(4, { 4: 'SCT-013-050, 50 A' })).toBe(50);
    expect(ctRatingA(2, { 2: 'CLAMP2000, 4.6 A' })).toBe(4.6);
  });
});

describe('sensor class and CT range (root 18.10)', () => {
  it('class 0 = not calibrated, 1 = SCT- (choosable), 2/3 = CT- (fixed)', () => {
    const { sensorClassOf, ctSettable } = jest.requireActual('./moduleLine');
    expect(sensorClassOf({ module: 0, present: true, sensor_class: 0 })).toBe('uncalibrated');
    expect(ctSettable({ module: 0, present: true, sensor_class: 0 })).toBe(false);
    expect(ctSettable({ module: 0, present: true, sensor_class: 1 })).toBe(true);
    expect(ctSettable({ module: 0, present: true, sensor_class: 2 })).toBe(false);
    expect(ctSettable({ module: 0, present: true, line: 'basic' })).toBe(true); // older canon: no class yet
  });
  it('accurate range: SCT- 1.5–90 %, CT- 4.3–100 %', () => {
    const { ctRange } = jest.requireActual('./moduleLine');
    expect(ctRange(50, 'sct')).toEqual({ lo: 0.75, hi: 45 });
    const ct = ctRange(100, 'ct');
    expect(ct.hi).toBe(100);
    expect(ct.lo).toBeCloseTo(4.3);
  });
});

describe('BASIC with a backwards CT (live .139 18.10: −520 W, energy flat)', () => {
  it('a negative p above the noise floor is a reversed sensor, not export', () => {
    const { basicReversed } = jest.requireActual('./moduleLine');
    expect(basicReversed(-518, 50, 226)).toBe(true);
    expect(basicReversed(-2.6, 50, 226)).toBe(false); // noise
    expect(basicReversed(518, 50, 226)).toBe(false);
    expect(basicReversed(-80, null, null)).toBe(true); // CT unknown → 50 W floor
  });
});
