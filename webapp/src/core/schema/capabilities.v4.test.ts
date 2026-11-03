import {
  deriveCapabilities, normalizeQualityBits, isOssBuild, isStorageMigrating,
  fieldsForSource, isFieldMeasured, RawSchema,
} from './capabilities';

describe('contract v4 schema keys', () => {
  // The live hub shape (hub .139, canon 7e0b1301, 2026-10-12).
  const liveSchema: RawSchema = {
    api_version: 1,
    fw: '1.0.2',
    build: 'full',
    hw_profile: 'esp32-wrover-psram',
    storage_state: 'ready',
    fields_by_source: {
      rbamp: ['v', 'i', 'p_act', 'v_min', 'v_max'],
      rbamp_std: ['v', 'i', 'p_act', 'p_rea', 'p_app', 'pf', 'f', 'thd_v', 'thd_i'],
      espnow: ['v', 'i', 'p_act'],
    },
    quality_bits: { PARTIAL: 0, TIME_UNSYNC: 1, MIGRATED: 8 }, // name→bit (live)
    endpoints: [],
  };

  it('surfaces build/hwProfile/storageState', () => {
    const caps = deriveCapabilities(liveSchema);
    expect(caps.build).toBe('full');
    expect(caps.hwProfile).toBe('esp32-wrover-psram');
    expect(caps.storageState).toBe('ready');
    expect(isOssBuild(caps)).toBe(false);
    expect(isStorageMigrating(caps)).toBe(false);
  });

  it('detects OSS build + migrating storage', () => {
    expect(isOssBuild(deriveCapabilities({ ...liveSchema, build: 'oss' }))).toBe(true);
    expect(isStorageMigrating(deriveCapabilities({ ...liveSchema, storage_state: 'migrating' }))).toBe(true);
  });

  it('normalizes quality_bits from either shape to bit→name', () => {
    expect(normalizeQualityBits({ PARTIAL: 0, MIGRATED: 8 })).toEqual({ 0: 'PARTIAL', 8: 'MIGRATED' }); // live
    expect(normalizeQualityBits({ '0': 'PARTIAL', '8': 'MIGRATED' })).toEqual({ 0: 'PARTIAL', 8: 'MIGRATED' }); // RC
    const caps = deriveCapabilities(liveSchema);
    expect(caps.qualityBits[0]).toBe('PARTIAL');
    expect(caps.qualityBits[8]).toBe('MIGRATED');
  });

  it('fields_by_source gating: hide only positively-unmeasured fields', () => {
    const caps = deriveCapabilities(liveSchema);
    expect(fieldsForSource(caps, 'rbamp')).toContain('p_act');
    // rbAmp has no pf/f → hidden (dash, not 0.00); block E.3 / DoD §9 #6
    expect(isFieldMeasured(caps, 'rbamp', 'pf')).toBe(false);
    expect(isFieldMeasured(caps, 'rbamp', 'p_act')).toBe(true);
    // a class that lists pf measures pf
    expect(isFieldMeasured(caps, 'rbamp_std', 'pf')).toBe(true);
    // unknown source → don't gate (unknown ≠ unmeasured)
    expect(fieldsForSource(caps, 'mystery')).toBeNull();
    expect(isFieldMeasured(caps, 'mystery', 'pf')).toBe(true);
    expect(isFieldMeasured(caps, undefined, 'pf')).toBe(true);
  });

  it('baseline (no schema) has empty v4 fields, gates nothing', () => {
    const caps = deriveCapabilities(null);
    expect(caps.build).toBeNull();
    expect(caps.fieldsBySource).toEqual({});
    expect(isFieldMeasured(caps, 'rbamp', 'pf')).toBe(true); // unknown → show
  });
});
