import { resolveSrcClass } from './telemetry-units';
import { deriveCapabilities, isFieldMeasured, RawSchema } from '../core/schema/capabilities';

const schema: RawSchema = {
  fields_by_source: {
    rbamp: ['v', 'i', 'p_act'],
    rbamp_std: ['v', 'i', 'p_act', 'pf', 'f'],
    espnow: ['v', 'i', 'p_act'],
  },
};

describe('resolveSrcClass (root canon 2026-10-12)', () => {
  it('prefers explicit src_class', () => {
    expect(resolveSrcClass({ src: 'i2c', src_class: 'rbamp_std' })).toBe('rbamp_std');
  });

  it('maps transport→class when src_class absent (temp map i2c→rbamp)', () => {
    expect(resolveSrcClass({ src: 'i2c' })).toBe('rbamp');
    expect(resolveSrcClass({ src: 'espnow' })).toBe('espnow');
  });

  it('passes an unknown transport through unchanged', () => {
    expect(resolveSrcClass({ src: 'mystery' })).toBe('mystery');
  });

  it('gates fields correctly through the resolved class', () => {
    const caps = deriveCapabilities(schema);
    // i2c unit → rbamp → no pf/f (block E.3: hide, not 0.00)
    expect(isFieldMeasured(caps, resolveSrcClass({ src: 'i2c' }), 'pf')).toBe(false);
    expect(isFieldMeasured(caps, resolveSrcClass({ src: 'i2c' }), 'p_act')).toBe(true);
    // a STD unit (explicit src_class) → has pf/f
    expect(isFieldMeasured(caps, resolveSrcClass({ src: 'i2c', src_class: 'rbamp_std' }), 'pf')).toBe(true);
  });
});
