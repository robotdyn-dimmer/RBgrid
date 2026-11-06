import { deriveCapabilities, BASELINE_CAPABILITIES } from '../core/schema/capabilities';
import { inputSeriesId, INPUT_SERIES } from './useInputSeries';

describe('input aggregate series (fw 1.1.6, root input-aggregate-api)', () => {
  it('reads the flag only from the hub’s explicit capabilities', () => {
    expect(deriveCapabilities({ endpoints: [{ path: '/api/v1/history', methods: 'GET' }] }).flags.inputAggregate).toBe(false);
    expect(deriveCapabilities({ endpoints: [], capabilities: { inputAggregate: true } }).flags.inputAggregate).toBe(true);
    expect(BASELINE_CAPABILITIES.flags.inputAggregate).toBe(false);
  });

  it('reads the input as `input` where the hub sums it, else its HEX4', () => {
    const on = deriveCapabilities({ endpoints: [], capabilities: { inputAggregate: true } });
    const off = deriveCapabilities({ endpoints: [] });
    expect(inputSeriesId(on, '0006')).toBe(INPUT_SERIES);
    expect(inputSeriesId(off, '0006')).toBe('0006');
    expect(inputSeriesId(on, null)).toBeNull();
  });
});
