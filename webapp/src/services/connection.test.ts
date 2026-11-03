import { connectDevice, deviceSameOrigin, disconnect } from './connection';

describe('deviceSameOrigin — X-FW-* only where no CORS preflight is needed', () => {
  afterEach(() => disconnect());

  it('a hub on another origin is cross-origin', () => {
    connectDevice('192.168.1.139');
    expect(deviceSameOrigin()).toBe(false);
  });

  it('the page served by the hub itself is same-origin', () => {
    connectDevice(window.location.origin);
    expect(deviceSameOrigin()).toBe(true);
  });
});
