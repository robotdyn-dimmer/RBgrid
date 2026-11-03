import { occupancyText } from './DevicesPage';
import { SensorListEntry, SensorModule } from '../types/sensors';

const s = (ch: number, unit_id: string | null): SensorListEntry => ({ iface: 'i2c', module: 0, ch, unit_id, present: true } as unknown as SensorListEntry);

describe('module occupancy — a module wired partly, any channels (operator 18.10)', () => {
  it('counts the bound current channels only: UI7 with I1, I4, I6', () => {
    const ui7 = { module: 0, present: true, n_i: 7 } as SensorModule;
    const list = [s(255, '0007'), s(0, null), s(1, '0101'), s(2, null), s(3, null), s(4, '0104'), s(5, null), s(6, '0106')];
    expect(occupancyText(ui7, list)).toMatch(/3.*7/);
  });
  it('says nothing for a single-channel module', () => {
    expect(occupancyText({ module: 0, present: true, n_i: 1 } as SensorModule, [s(0, '0006')])).toBe('');
  });
});
