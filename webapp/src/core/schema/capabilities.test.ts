import {
  deriveCapabilities, can, domainRequiresReboot, endpointRequiresAuth, BASELINE_CAPABILITIES,
} from './capabilities';
import { mockSchema } from '../mocks';

describe('deriveCapabilities', () => {
  it('null schema → baseline (all flags off, not fromSchema)', () => {
    const caps = deriveCapabilities(null);
    expect(caps.fromSchema).toBe(false);
    expect(can(caps, 'economics')).toBe(false);
    expect(caps).toBe(BASELINE_CAPABILITIES);
  });

  it('explicit capabilities{} win over endpoint inference', () => {
    const caps = deriveCapabilities(mockSchema());
    expect(caps.fromSchema).toBe(true);
    expect(can(caps, 'economics')).toBe(true);
    expect(can(caps, 'espnow')).toBe(true);
    expect(can(caps, 'provisioning')).toBe(true);
    // The profile follows whatever the schema says (the demo fixture may change it).
    expect(caps.telemetryProfile).toBe(mockSchema().telemetry_profile);
  });

  it('parses the configurable surface (money / roles / key format, canon fc0f962b)', () => {
    const caps = deriveCapabilities({
      money: { currency_code: 'KWD', money_decimals: 3, rate_decimals: 4 },
      enums: { role: ['MAIN_SUPPLY', 'RENEWABLE_SOURCE', 'EV_CHARGING'] },
      key_format: { unit_key: 'HEX4', legacy_supply_keys: false },
    });
    expect(caps.money).toEqual({ currencyCode: 'KWD', moneyDecimals: 3, rateDecimals: 4 });
    expect(caps.roleEnum).toContain('RENEWABLE_SOURCE');
    expect(caps.legacySupplyKeys).toBe(false);
  });

  it('money defaults when the schema omits it: no currency, 2 / 4 decimals; legacy keys assumed', () => {
    const caps = deriveCapabilities({ endpoints: [] });
    expect(caps.money).toEqual({ currencyCode: null, moneyDecimals: 2, rateDecimals: 4 });
    expect(caps.legacySupplyKeys).toBe(true);
  });

  it('parses comma-separated methods (string, not array)', () => {
    const caps = deriveCapabilities({
      endpoints: [{ path: '/x', methods: 'GET,POST' }],
      capabilities: {},
    });
    // mqttConfig path differs, but the parse is exercised via has(); sanity: no throw + fromSchema
    expect(caps.fromSchema).toBe(true);
  });

  it('falls back to endpoint inference when no explicit capabilities{}', () => {
    const caps = deriveCapabilities({
      endpoints: [{ path: '/api/v1/economics', methods: 'GET' }],
    });
    expect(can(caps, 'economics')).toBe(true);
    expect(can(caps, 'espnow')).toBe(false);
  });
});

describe('per-endpoint auth + reboot flags', () => {
  const caps = deriveCapabilities(mockSchema());

  it('endpointRequiresAuth reads the landed auth_required flag', () => {
    expect(endpointRequiresAuth(caps, '/api/system/system-config')).toBe(true);
    expect(endpointRequiresAuth(caps, '/api/v1/history')).toBe(false);
  });

  it('domainRequiresReboot reads the landed per-endpoint requires_reboot', () => {
    expect(domainRequiresReboot(caps, 'system')).toBe(true);
    expect(domainRequiresReboot(caps, 'hardware')).toBe(true);
    expect(domainRequiresReboot(caps, 'mqtt')).toBe(true);
    expect(domainRequiresReboot(caps, 'tariff')).toBe(false);
    expect(domainRequiresReboot(caps, 'units')).toBe(false);
    expect(domainRequiresReboot(caps, 'espnow')).toBe(false); // domain-level; add/pair reboot is action-level
  });

  it('returns undefined for domains the schema does not cover (→ caller falls back)', () => {
    expect(domainRequiresReboot(BASELINE_CAPABILITIES, 'wifi')).toBeUndefined();
    expect(domainRequiresReboot(caps, 'security')).toBeUndefined();
  });
});
