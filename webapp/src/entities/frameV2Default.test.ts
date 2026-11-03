import { normalizeTelemetryFrame } from './model';
import frame from './__fixtures__/frame-v2-default.json';

// v1.1.0: the hub sends frame v2 by default — WS without `?v=2`, /api/telemetry/latest without a
// parameter (root 20.10, df1ce360; this fixture is .139's /api/telemetry/latest as served).
describe('frame v2 by default', () => {
  const n = normalizeTelemetryFrame(frame as never);
  it('maps the voltage bus to v_bus and keeps the hex keys of the points', () => {
    const keys = n.units.map((g) => g.unitKey);
    expect(keys).toContain('v_bus');
    expect(keys).toContain('0006');
    expect(keys).not.toContain('0007');
  });
  it('carries the hardware revision the app re-reads /api/v1/sensors on', () => {
    expect(typeof (frame as { hw_rev?: unknown }).hw_rev).toBe('number');
    expect(typeof (frame as { boot_id?: unknown }).boot_id).toBe('number');
  });
});
