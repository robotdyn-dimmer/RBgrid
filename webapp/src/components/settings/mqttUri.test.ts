import { parseBrokerUri, buildBrokerUri, EMPTY_BROKER } from './mqttUri';

describe('broker URI ⇄ form', () => {
  it('reads a plain URI and the default port', () => {
    expect(parseBrokerUri('mqtt://192.168.1.100:1883')).toMatchObject({ tls: false, host: '192.168.1.100', port: '1883', user: '', maskedCreds: false });
    expect(parseBrokerUri('mqtts://broker.local')).toMatchObject({ tls: true, port: '8883' });
  });
  it('a masked GET value keeps no credentials and says they are hidden', () => {
    expect(parseBrokerUri('mqtts://***@ha.local:8883')).toMatchObject({ host: 'ha.local', user: '', pass: '', maskedCreds: true });
  });
  it('builds the URI the hub accepts, escaping credentials', () => {
    expect(buildBrokerUri({ ...EMPTY_BROKER, host: 'ha.local', port: '1883' })).toBe('mqtt://ha.local:1883');
    expect(buildBrokerUri({ ...EMPTY_BROKER, tls: true, host: 'ha.local', port: '8883', user: 'rb', pass: 'p@ss:1' }))
      .toBe('mqtts://rb:p%40ss%3A1@ha.local:8883');
    expect(parseBrokerUri('mqtts://rb:p%40ss%3A1@ha.local:8883')).toMatchObject({ user: 'rb', pass: 'p@ss:1' });
  });
  it('refuses what the hub would refuse', () => {
    expect(buildBrokerUri({ ...EMPTY_BROKER, host: '' })).toBeNull();
    expect(buildBrokerUri({ ...EMPTY_BROKER, host: 'a b' })).toBeNull();
    expect(buildBrokerUri({ ...EMPTY_BROKER, host: 'h', port: '70000' })).toBeNull();
    expect(buildBrokerUri({ ...EMPTY_BROKER, host: 'h', pass: 'x' })).toBeNull();
  });
});
