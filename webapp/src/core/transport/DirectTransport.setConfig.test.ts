import { AxiosRequestConfig, AxiosResponse } from 'axios';
import { DirectTransport, buildConfigPostBody, extractConfigPayload, InvalidConfigDocError } from './DirectTransport';
import { UnsupportedDomainError } from './types';
import { getApiClient } from '../../services/api';
import { connectDevice } from '../../services/connection';

/**
 * setConfig must POST exactly what each firmware handler parses (B-2). Bodies below mirror
 * the live GETs of the sandbox hub .139 (fw 1.0.2, 2026-10-13); the system round-trip shape was
 * accepted live by POST /api/system/system-config (200 OK), the bare inner doc got 400
 * MISSING_SYSTEM_SECTION.
 */

const SYSTEM_GET = {
  system: {
    version: '1.0.0', device_id: 'ESP32-RBGrid', home_name: 'Default', location: 'Unknown', language: 'EN',
    logging_level: 3, enable_logging: false, enable_telemetry: false, max_retry_attempts: 0, retry_delay_ms: 0,
    enable_json_compression: false, enable_encryption: false, auto_start_measurements: true,
    site_profile: 'home', ev: { charge_threshold_w: 0, idle_timeout_min: 0, price_per_kwh: 0 },
  },
};
const META = { version: '2.0', device_id: 'rbgrid_main_001', timestamp_local: 849, timestamp_ntp: 1791849983 };
const UNITS_GET = { type: 'units_config', ...META, data: { units: [{ unit_id: 7, name: 'Main Voltage Bus', role: 'MAIN_SUPPLY' }], units_count: 1 } };
const TARIFF_GET = { type: 'tariff_plan_list_config', ...META, data: { list: [{ plan_name: 'Test Day/Night', zones: [] }] } };
const TELEMETRY_GET = { type: 'telemetry_config', ...META, data: { telemetry_enabled: true, channels: [] }, profile: 'full' };

let posts: Array<{ url: string; data: unknown; headers: Record<string, unknown> }> = [];

beforeAll(() => {
  connectDevice('192.168.1.139');
  getApiClient().getAxiosInstance().defaults.adapter = async (config: AxiosRequestConfig): Promise<AxiosResponse> => {
    posts.push({
      url: String(config.url),
      data: typeof config.data === 'string' ? JSON.parse(config.data) : config.data,
      headers: (config.headers ?? {}) as Record<string, unknown>,
    });
    return { data: { status: 'OK' }, status: 200, statusText: 'OK', headers: {}, config } as AxiosResponse;
  };
});
beforeEach(() => { posts = []; });

describe('DirectTransport.setConfig — POST body per domain (round-trip of the GET shape)', () => {
  const t = new DirectTransport();

  it('system → {"system":{…}} (G1 site_profile / G7 ev edit)', async () => {
    const inner = extractConfigPayload(SYSTEM_GET);
    await t.setConfig('system', { ...inner, site_profile: 'industrial', ev: { charge_threshold_w: 1500, idle_timeout_min: 10, price_per_kwh: 0.25 } });
    expect(posts).toHaveLength(1);
    expect(posts[0].url).toBe('/api/system/system-config');
    expect(posts[0].data).toEqual({
      system: { ...SYSTEM_GET.system, site_profile: 'industrial', ev: { charge_threshold_w: 1500, idle_timeout_min: 10, price_per_kwh: 0.25 } },
    });
  });

  it('system with an unchanged doc round-trips to exactly the GET body', async () => {
    await t.setConfig('system', extractConfigPayload(SYSTEM_GET));
    expect(posts[0].data).toEqual(SYSTEM_GET);
  });

  it('units → {"type":"units_config","data":{…}}', async () => {
    await t.setConfig('units', extractConfigPayload(UNITS_GET));
    expect(posts[0].url).toBe('/api/system/units-config');
    expect(posts[0].data).toEqual({ type: 'units_config', data: UNITS_GET.data });
  });

  it('tariff → {"type":"tariff_plan_list_config","data":{"list":[…]}}', async () => {
    await t.setConfig('tariff', extractConfigPayload(TARIFF_GET));
    expect(posts[0].url).toBe('/api/system/tariff-plan-config');
    expect(posts[0].data).toEqual({ type: 'tariff_plan_list_config', data: TARIFF_GET.data });
  });

  it('telemetry → {"profile":…} only (root-level profile survives extractConfigPayload)', async () => {
    const inner = extractConfigPayload(TELEMETRY_GET);
    expect(inner.profile).toBe('full');
    await t.setConfig('telemetry', { ...inner, profile: 'lean' });
    expect(posts[0].url).toBe('/api/system/telemetry-config');
    expect(posts[0].data).toEqual({ profile: 'lean' });
  });

  it('telemetry without a valid profile is rejected before the network', async () => {
    await expect(t.setConfig('telemetry', { telemetry_enabled: true })).rejects.toBeInstanceOf(InvalidConfigDocError);
    expect(posts).toHaveLength(0);
  });

  it.each(['time', 'wifi', 'mqtt', 'espnow', 'security'] as const)(
    '%s has no writable REST route → UnsupportedDomainError, no request', async (d) => {
      await expect(t.setConfig(d as never, {})).rejects.toBeInstanceOf(UnsupportedDomainError);
      expect(posts).toHaveLength(0);
    },
  );

  it('buildConfigPostBody is the single source of the shapes', () => {
    expect(buildConfigPostBody('system', { a: 1 })).toEqual({ system: { a: 1 } });
    expect(buildConfigPostBody('time', {})).toBeNull();
    // hardware round-trips the GET envelope (canon 2beb4bc8)
    expect(buildConfigPostBody('hardware', { network: { nominal_voltage: 230 } })).toEqual({ type: 'hardware_config', data: { network: { nominal_voltage: 230 } } });
  });
});
