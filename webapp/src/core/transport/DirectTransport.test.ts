import { extractConfigPayload } from './DirectTransport';

/**
 * Real config-GET envelope shapes captured from the live hub (http://192.168.1.110,
 * fw 1.0.2, 2026-06-29). The device uses THREE shapes — extractConfigPayload must
 * normalize all three to the inner config object the shadow/editor consumes.
 */
describe('extractConfigPayload — real device envelopes', () => {
  it('base-wrapper {type,…,data:{…}} → returns data (time/hardware/tariff/units/telemetry/wifi)', () => {
    const body = {
      type: 'time_config', version: '2.0', device_id: 'rbgrid_main_001',
      timestamp_local: 68223, timestamp_ntp: 1782715645,
      data: { config: { enable_time_sync: true }, statistics: {} },
    };
    expect(extractConfigPayload(body)).toEqual({ config: { enable_time_sync: true }, statistics: {} });
  });

  it('section-keyed {system:{…}} → returns the section object (system-config)', () => {
    const body = { system: { home_name: 'Default', location: 'Unknown', language: 'EN', logging_level: 3 } };
    expect(extractConfigPayload(body)).toEqual({ home_name: 'Default', location: 'Unknown', language: 'EN', logging_level: 3 });
  });

  it('bare object {broker_uri,…} → returns it as-is (mqtt-config)', () => {
    const body = { broker_uri: 'mqtt://192.168.1.101:1883', persisted: true, connected: true, note: 'x' };
    expect(extractConfigPayload(body)).toEqual(body);
  });

  it('bare object with envelope meta → strips meta, keeps payload', () => {
    const body = { type: 'x', version: '2.0', device_id: 'd', timestamp_local: 1, timestamp_ntp: 2, broker_uri: 'mqtt://h:1883', connected: false };
    expect(extractConfigPayload(body)).toEqual({ broker_uri: 'mqtt://h:1883', connected: false });
  });

  it('handles empty / non-object defensively', () => {
    expect(extractConfigPayload(null)).toEqual({});
    expect(extractConfigPayload(undefined)).toEqual({});
    expect(extractConfigPayload('nope')).toEqual({});
  });

  it('does not treat a single ARRAY-valued key as the payload', () => {
    // {data:[…]} only unwraps when data is an object; an array stays under its key
    const body = { sensors: [{ a: 1 }] };
    expect(extractConfigPayload(body)).toEqual({ sensors: [{ a: 1 }] });
  });
});
