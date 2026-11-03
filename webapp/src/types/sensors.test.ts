import { freeSensors, SensorListEntry, SensorModule } from './sensors';

const s = (unit_id: string | null, ch: number, addr = 80): SensorListEntry =>
  ({ unit_id, present: true, iface: 'i2c', src_class: 'rbamp', bus: 0, module: 0, ch, addr } as unknown as SensorListEntry);

describe('freeSensors', () => {
  it('hides a free entry on a channel a point already uses (.191, 17.10)', () => {
    expect(freeSensors([s('0006', 0), s(null, 0)])).toEqual([]);
  });
  it('hides channels the fitted module lacks — UI3 in place of UI7 (.139, 18.10)', () => {
    const ui3 = [{ module: 0, present: true, n_i: 3 }] as SensorModule[];
    const list = [s(null, 255), s(null, 2), { ...s(null, 3), present: false }, { ...s(null, 6), present: false }];
    expect(freeSensors(list, ui3).map((x) => x.ch)).toEqual([255, 2]);
    expect(freeSensors(list).map((x) => x.ch)).toEqual([255, 2, 3, 6]);
  });
  it('keeps truly free channels', () => {
    expect(freeSensors([s('0006', 0), s(null, 1), s(null, 0, 81)]).map((x) => `${x.addr}/${x.ch}`)).toEqual(['80/1', '81/0']);
  });
});
