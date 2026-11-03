import { evThresholds, stationThresholds, deriveStationStatus, EV_DEFAULTS } from './evStatus';

describe('station thresholds: station → hub → defaults (root 56be1d45)', () => {
  it('a station value wins, 0 / absent falls back to the hub, then to the defaults', () => {
    const hub = evThresholds({ charge_threshold_w: 800, idle_timeout_min: 0, price_per_kwh: 0 });
    expect(hub).toEqual({ chargeThresholdW: 800, idleTimeoutMin: EV_DEFAULTS.idleTimeoutMin });
    expect(stationThresholds(hub, { chargeThresholdW: 1500, idleTimeoutMin: 20 })).toEqual({ chargeThresholdW: 1500, idleTimeoutMin: 20 });
    expect(stationThresholds(hub, { chargeThresholdW: 0 })).toEqual(hub);
    expect(stationThresholds(hub)).toEqual(hub);
  });
  it('the station threshold decides «charging»', () => {
    const hub = evThresholds();
    expect(deriveStationStatus(1000, true, null, hub)).toBe('charging');
    expect(deriveStationStatus(1000, true, null, stationThresholds(hub, { chargeThresholdW: 1500 }))).not.toBe('charging');
  });
});
