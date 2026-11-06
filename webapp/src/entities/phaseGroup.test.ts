import { inputGroupOffer, inputMetering, PHASE_GROUP_CH } from './phaseGroup';
import type { SensorListEntry, SensorModule } from '../types/sensors';

const ch = (module: number, c: number, unit_id: string | null = null): SensorListEntry =>
  ({ iface: 'i2c', present: true, unit_id, bus: 0, module, ch: c, addr: 0x50 + module });
const u3i3: SensorModule = { module: 0, addr: 0x50, present: true, line: 'std', model: 'STD-U3I3', n_v: 3, n_i: 3 };
const i2: SensorModule = { module: 2, addr: 0x52, present: true, line: 'basic', model: 'BASIC-I2', n_v: 0, n_i: 2 };

describe('the input on all three phases of a U3I3 (rebind, v1.1.2)', () => {
  const free = [ch(0, 0, '0006'), ch(2, 0), ch(2, 1)];
  it('a present phase module: regroup on 1.1.2+, an update caption below', () => {
    expect(Array.from(inputGroupOffer(free, [u3i3, i2], '0006', true))).toEqual([[0, 'regroup']]); // BASIC-I2 is not a phase module
    expect(Array.from(inputGroupOffer(free, [u3i3], '0006', false))).toEqual([[0, 'update']]);
  });
  it('nothing when the version or the input is unknown, the module is absent, or the input is grouped already', () => {
    expect(inputGroupOffer(free, [u3i3], '0006', null).size).toBe(0);
    expect(inputGroupOffer(free, [u3i3], null, true).size).toBe(0);
    expect(inputGroupOffer(free, [{ ...u3i3, present: false }], '0006', true).size).toBe(0);
    expect(inputGroupOffer([ch(0, PHASE_GROUP_CH, '0006')], [u3i3], '0006', true).size).toBe(0);
  });
});

describe('channel as printed on the module (root 16.10)', () => {
  // eslint-disable-next-line @typescript-eslint/no-var-requires
  const { channelLabel } = require('./phaseGroup');
  it('current channels from 0, the grouped input and the voltage input; the hub label wins', () => {
    expect(channelLabel(0)).toBe('I0');
    expect(channelLabel(254)).toBe('L1–L3');
    expect(channelLabel(255)).toBe('V');
    expect(channelLabel(1, 'L2')).toBe('L2');
    expect(channelLabel(undefined)).toBe('—');
  });
});

describe('the input on a U3I3: netted or per phase (fw 1.1.5)', () => {
  const u3i3 = { module: 2, present: true, n_v: 3, n_i: 3, model: 'STD-U3I3' } as SensorModule;
  const basic = { module: 1, present: true, n_v: 1, n_i: 3, model: 'BASIC-UI3' } as SensorModule;
  const s = (module: number, ch: number, unit_id: string | null) => ({ iface: 'i2c', bus: 0, module, ch, unit_id, present: true }) as SensorListEntry;
  const roles: Record<string, string> = { '0006': 'MAIN_SUPPLY', '0103': 'MAIN_SUPPLY', '0104': 'MAIN_SUPPLY', '0102': 'DEVICE_LOAD' };
  const roleOf = (id: string) => roles[id];

  it('split, as on .139 06.11: the input on L1, L2/L3 its legs', () => {
    const m = inputMetering([s(2, 0, '0006'), s(2, 1, '0103'), s(2, 2, '0104'), s(1, 0, '0102'), s(1, 1, null)], [u3i3, basic], '0006', roleOf);
    expect(m.get(2)).toMatchObject({ mode: 'split', inputCh: 0, legs: ['0103', '0104'], freePhases: [] });
    expect(m.has(1)).toBe(false);
  });
  it('grouped on L1–L3', () => {
    expect(inputMetering([s(2, PHASE_GROUP_CH, '0006')], [u3i3], '0006', roleOf).get(2)).toMatchObject({ mode: 'grouped', legs: [] });
  });
  it('the input on L1 alone: the free phases are offered for points', () => {
    expect(inputMetering([s(2, 0, '0006'), s(2, 1, null), s(2, 2, null)], [u3i3], '0006', roleOf).get(2)).toMatchObject({ mode: 'single', freePhases: [1, 2] });
  });
  it('a line on another phase is not a leg; an input on a BASIC is not offered', () => {
    expect(inputMetering([s(2, 0, '0006'), s(2, 1, '0102')], [u3i3], '0006', roleOf).get(2)).toMatchObject({ mode: 'single', legs: [], freePhases: [2] });
    expect(inputMetering([s(1, 0, '0006')], [basic], '0006', roleOf).size).toBe(0);
  });
});
