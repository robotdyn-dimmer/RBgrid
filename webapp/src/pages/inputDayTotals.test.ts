import { inputDayTotals, todaySeed } from './overviewData';
import type { UnitEntityGroup } from '../entities/model';

/** Kimi #5: every running-period total takes «today» by one rule — an rbAmp input's own day
 *  counter where the hub's system sums stay 0 (hubs before T12). */
const rbamp = (dayIn: number, dayOut = 0) => ({
  unitKey: '0A1F',
  entities: [
    { entity_id: 'sensor.rbgrid_0A1F_energy_day_in', state: dayIn },
    { entity_id: 'sensor.rbgrid_0A1F_energy_day_out', state: dayOut },
  ],
}) as unknown as UnitEntityGroup;

describe('inputDayTotals', () => {
  it('system 0 + rbAmp > 0 → the rbAmp counter, money unknown (not a fake 0)', () => {
    const t = inputDayTotals({ e_day_in: 0, e_day_out: 0, cost_day: 0, rev_day: 0 } as never, rbamp(5300));
    expect(t?.e_day_in).toBe(5300);
    expect(t?.cost_day).toBeNull();
    expect(todaySeed(t)).toEqual({ inWh: 5300, outWh: 0, cost: 0, revenue: 0, n: 1 });
  });
  it('a hub that sums the rbAmp input → its own figures, money kept', () => {
    const t = inputDayTotals({ e_day_in: 5300, e_day_out: 0, cost_day: 1.93, rev_day: 0 } as never, rbamp(5300));
    expect(t?.e_day_in).toBe(5300);
    expect(t?.cost_day).toBe(1.93);
  });
  it('a legacy ADC input → the system sums', () => {
    const legacy = { ...rbamp(0), unitKey: 'main' } as UnitEntityGroup;
    expect(inputDayTotals({ e_day_in: 800, cost_day: 0.3 } as never, legacy)?.e_day_in).toBe(800);
  });
  it('no day counter → no seed', () => {
    expect(todaySeed(undefined)).toBeNull();
  });
});
