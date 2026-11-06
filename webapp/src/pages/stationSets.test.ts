/**
 * A three-phase charger bound as three points (one per conductor) in one group is ONE station:
 * one card, one balance segment, one session (content use-cases-persona-facts #3).
 */
import { normalizeTelemetryFrame } from '../entities/model';
import { buildUnitsJoin } from '../hooks/useUnitsConfig';
import { buildStations, buildLoadSegments, stationCounts, stationSets } from './overviewData';
import { countSessions, stationSeries } from '../components/stations/ParkingPowerChart';
import type { JsonObject } from '../core/json';

const join = buildUnitsJoin([
  { unit_id: '9001', role: 'MAIN_SUPPLY', phase: 4 },
  { unit_id: '9031', point: 'c1', name: 'Charger 3P', role: 'EV_CHARGING', phase: 1, group_id: 2, rated_power_w: 3700 },
  { unit_id: '9032', point: 'c2', name: 'Charger L2', role: 'EV_CHARGING', phase: 2, group_id: 2, rated_power_w: 3700 },
  { unit_id: '9033', point: 'c3', name: 'Charger L3', role: 'EV_CHARGING', phase: 3, group_id: 2, rated_power_w: 3700 },
  { unit_id: '9040', point: 'ac', name: 'AC charger', role: 'EV_CHARGING', phase: 1 },
] as unknown as JsonObject[]);
const data = normalizeTelemetryFrame({
  v: 1, ts: 0, freq: 50, net_pwr: 0, net_dir: 0,
  units: {
    '9001': { src: 'i2c', present: true, i: 30, p_act: 7000 },
    '9031': { src: 'i2c', present: true, point: 'c1', i: 10, p_act: 2300 },
    '9032': { src: 'i2c', present: true, point: 'c2', i: 10, p_act: 2300 },
    '9033': { src: 'i2c', present: true, point: 'c3', i: 10, p_act: 2300 },
    '9040': { src: 'i2c', present: true, point: 'ac', i: 0, p_act: 0 },
  },
} as never);

describe('stations from grouped points', () => {
  it('three points in one group make one station, an ungrouped point its own', () => {
    expect(stationSets(data.units, join).map((s) => s.keys)).toEqual([['9031', '9032', '9033'], ['9040']]);
    const st = buildStations(data.units, join, { chargeThresholdW: 1000, idleTimeoutMin: 10 }, new Map([[2, 'Fast charger']]));
    expect(st).toHaveLength(2);
    expect(st[0]).toMatchObject({ id: '9031', name: 'Fast charger', powerW: 6900, status: 'charging', maxPowerW: 11100 });
    expect(stationCounts(st)).toEqual({ charging: 1, free: 1, total: 2 });
    expect(buildLoadSegments(data.units, join).filter((s) => s.kind === 'station').map((s) => s.valueW)).toEqual([6900, 0]);
  });

  it('separate chargers in one group (a car park\'s «Charging stations») stay separate stations', () => {
    const j = buildUnitsJoin([
      { unit_id: '9001', role: 'MAIN_SUPPLY', phase: 1 },
      { unit_id: '9030', point: 'e1', role: 'EV_CHARGING', phase: 1, group_id: 1, rated_power_w: 7400 },
      { unit_id: '9031', point: 'e2', role: 'EV_CHARGING', phase: 1, group_id: 1, rated_power_w: 7400 },
      { unit_id: '9032', point: 'e3', role: 'EV_CHARGING', phase: 1, group_id: 1, rated_power_w: 7400 },
    ] as unknown as JsonObject[]);
    const d = normalizeTelemetryFrame({
      v: 1, ts: 0, freq: 50, net_pwr: 0, net_dir: 0,
      units: Object.fromEntries(['9001', '9030', '9031', '9032'].map((k, i) => [k, { src: 'i2c', present: true, point: ['', 'e1', 'e2', 'e3'][i], i: 1, p_act: 100 }])),
    } as never);
    expect(stationSets(d.units, j).map((s) => s.keys)).toEqual([['9030'], ['9031'], ['9032']]);
  });

  it('one session on three conductors counts once', () => {
    const from = 0;
    const phase = [{ ts: 0, p_act: 0 }, { ts: 3600, p_act: 2300 }, { ts: 7200, p_act: 2300 }, { ts: 10800, p_act: 0 }];
    expect(countSessions(stationSeries([phase, phase, phase], from), 1000)).toBe(1);
    // 500 W per conductor is below the threshold on each point, but the station draws 1.5 kW
    const low = phase.map((r) => ({ ...r, p_act: r.p_act ? 500 : 0 }));
    expect(countSessions(stationSeries([low, low, low], from), 1000)).toBe(1);
  });
});
