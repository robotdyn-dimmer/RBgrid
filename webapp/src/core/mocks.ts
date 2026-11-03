/**
 * Mock-first scaffolding for the NEW endpoints. Shapes are now grounded on root's
 * confirmed contract (2026-06-23) — these match the real firmware handlers, so the
 * UI built against them re-points to the live device with zero divergence.
 */
import { RawSchema } from './schema/capabilities';
import { JsonObject } from './json';
import { ConfigDomain } from './shadow/ConfigShadow';
import { ProvisioningStatus } from './provisioning/ProvisioningFSM';
import { Economics, Summary, TARIFF_ZONES } from './economics/types';
import { EspnowStatus } from './espnow/types';
import { demoRegion, dm, demoPosixTz, demoTariff, demoZoneMoney, DEMO_CT_MODELS } from '../mock/demo';
import { demoSite } from '../mock/demoSites';
import { demoVariant } from '../mock/demoVariant';

/** Mock GET /api/v1/espnow — a hub with one online node (two channels bound). */
export function mockEspnow(): EspnowStatus {
  return {
    running: true,
    channel: 6,
    pairing_open: false,
    paired_count: 1,
    beacons_sent: 1280,
    rt_recv: 1240,
    stats_recv: 410,
    nodes: [
      {
        mac: 'A4:CF:12:00:7E:01',
        online: true,
        disabled: false,
        last_seen_s: 2,
        sync: { resid_sd_us: 38, skew_ppm: 1.2, loss_pct: 0.4, fits: 512 },
        channels: [
          { ch: 0, ct_code: 3, unit_id: '0x6A00', present: true },
          { ch: 1, ct_code: 6, unit_id: '0x6A01', present: true },
        ],
      },
    ],
  };
}

/**
 * Minimal config-domain shapes for demo mode (so the reconciliation editor is
 * demonstrable without a device). Representative — mixed primitive types to exercise
 * the generic editor. Real-device shapes (v1 /api/system/*-config) are richer and
 * verified on the bench; the editor is shape-agnostic so it adapts either way.
 */
export function mockConfig(domain: ConfigDomain): JsonObject {
  switch (domain) {
    case 'system':
      // site_profile (G1) + ev (G7) are mock-first here (land on-device next canon).
      return {
        // The variant's site (mock/demoVariant): home / small business / car park.
        home_name: demoSite().title,
        location: 'Demo', language: '', enable_home_network: true,
        site_profile: demoVariant().profile,
        // Group names (root e139ab5e); membership is units[].group_id.
        groups: demoSite().groups.map((g) => ({ id: g.id, name: g.name })),
        ev: { charge_threshold_w: 500, idle_timeout_min: 30, price_per_kwh: dm(12) },
        // Mains system (root canon bbf06397, delta №4): only `system` is writable; nominal_v/_hz
        // mirror hardware-config network. US demo declares SPLIT; the default leaves it unset
        // ("" = not set → single-phase, as the hub counts it — the EU demo is a single-phase home).
        grid: { system: demoRegion().us ? 'SPLIT' : demoSite().points.some((p) => p.phase === 4) ? '3P' : '', nominal_v: demoRegion().v, nominal_hz: demoRegion().hz },
      };
    case 'time':
      return { ntp_enabled: true, ntp_server: 'pool.ntp.org', timezone: demoPosixTz(), auto_dst: false }; // the viewer's zone (demo history profiles are drawn in it)
    case 'hardware':
      return {
        network: { nominal_voltage: demoRegion().v, nominal_frequency: demoRegion().hz }, // delta №4
      };
    case 'telemetry':
      return { telemetry_enabled: true, compact_json: true, json_buffer_size: 4096 };
    case 'mqtt':
      return { broker_uri: 'mqtt://192.168.1.101:1883', persisted: true, connected: false };
    case 'tariff':
      return { plan_name: demoTariff().plan, currency_code: demoRegion().cur, vat_percent: demoTariff().vat, net_metering_enabled: demoTariff().sell != null };
    case 'units': {
      // Per-unit config shaped like the real hub (clean install, no native ADC — root f451b6a5),
      // built from the variant's site (mock/demoSites): the input is an rbAmp unit with role
      // MAIN_SUPPLY keyed by HEX4; firmware role enum; 0 = not set. The input's rated_power_w is
      // the input limit (G4). short_name ≤ schema.limits.short_name_bytes (24 B UTF-8).
      const site = demoSite();
      const v = demoRegion().v;
      return {
        units_count: site.points.length + 1, active_units_count: site.points.filter((p) => !p.absent).length, system_healthy: true, auto_balance: false,
        units: site.points.map((p) => ({
          unit_id: parseInt(p.id, 16), point: p.point, name: p.name, short_name: p.short, type: 'CURRENT_LOAD', role: p.role,
          ...(p.group ? { group_id: p.group, role_trunk: 0 } : {}),
          ...(p.role === 'EV_CHARGING' ? { charge_threshold_w: 0, idle_timeout_min: 0 } : {}),
          phase: p.phase, rated_power_w: p.ratedW, max_current_a: p.maxA, rated_voltage_v: p.phase === 5 ? v * 2 : v,
        })),
      };
    }
    default:
      return {};
  }
}

/** Mock GET /api/system/provisioning — a partially-onboarded device (wifi+time done). */
export function mockProvisioning(over: Partial<ProvisioningStatus> = {}): ProvisioningStatus {
  return {
    wifi_provisioned: true,
    time_synced: true,
    hardware_configured: false,
    units_bound: 0,
    nodes_paired: 0,
    tariff_set: false,
    token_set: false,
    https_enabled: false,
    device_ready: false,
    ...over,
  };
}

/**
 * A schema matching the LANDED post-2026-06-23 firmware shape (BARE): explicit
 * `capabilities{}` + per-endpoint `auth_required`/`requires_reboot` (HW-validated by root).
 */
export function mockSchema(): RawSchema {
  return {
    api_version: 1,
    telemetry_schema: 1,
    telemetry_profile: 'full',
    fw: 'mock-2026.10',
    // contract-v4 additive keys (mirrors the live hub .139 shapes) so schema-gating demos.
    build: 'full',
    hw_profile: 'esp32-wrover-psram',
    storage_state: 'ready',
    fields_by_source: {
      rbamp: ['v', 'i', 'p_act', 'v_min', 'v_max', 'i_min', 'i_max', 'p_min', 'p_max'],
      espnow: ['v', 'i', 'p_act'],
    },
    quality_bits: { PARTIAL: 0, TIME_UNSYNC: 1, CLOCK_STEP: 2, SRC_GAP: 3, RECOMPUTED: 4, ESTIMATED: 5, UNSIGNED_SRC: 6, OVERRANGE: 7, MIGRATED: 8 },
    transports: { ws: '/ws-telemetry', mqtt: 'rbgrid/<mac>/telemetry' },
    // configurable surface (canon fc0f962b — same shape as GET /api/v1/schema on .139)
    units_of_measure: { energy: 'Wh', power: 'W', voltage: 'V', current: 'A', frequency: 'Hz', money: 'major', rate: 'major_per_kwh' },
    key_format: { unit_key: 'HEX4', legacy_supply_keys: false }, // clean install: no main/alt/v_bus blocks
    limits: { short_name_bytes: 24, name_bytes: 31, origins_max: 8 }, // canon f451b6a5
    enums: {
      role: ['MAIN_SUPPLY', 'ALTERNATIVE_SUPPLY', 'CIRCUIT_BREAKER', 'DEVICE_LOAD', 'ZONE_LOAD', 'RENEWABLE_SOURCE', 'STORAGE_SYSTEM', 'EV_CHARGING', 'HEATING_COOLING', 'SMART_APPLIANCE'],
      site_profile: ['home', 'industrial', 'ev'],
      ct_model: DEMO_CT_MODELS,
      language: ['RU', 'EN'], // canon cfa380bd: system.language "" | RU | EN
      phase: { 0: 'UNSET', 1: 'L1', 2: 'L2', 3: 'L3', 4: '3P', 5: 'L1L2', 6: 'L2L3', 7: 'L3L1' },
      grid_system: ['1P', 'SPLIT', '3P'],
      grid_system_validated: ['1P', '3P'], // F20; 3P validated on the live 3φ run 16.10 (operator)
      tariff_zone: ['NIGHT', 'MORNING_PEAK', 'DAY', 'EVENING_PEAK', 'LATE_EVENING', 'WEEKEND', 'HOLIDAY', 'CRITICAL_PEAK'],
      tariff_plan_type: ['SIMPLE', 'TWO_ZONE', 'THREE_ZONE', 'MULTI_ZONE', 'PROGRESSIVE', 'SEASONAL', 'DYNAMIC', 'CUSTOM'],
      tariff_condition_type: ['TIME_RANGE', 'WEEKDAY', 'WEEKEND', 'HOLIDAY', 'SEASON', 'MONTH', 'DATE_RANGE', 'SPECIAL_EVENT'],
    },
    money: { currency_code: demoRegion().cur, money_decimals: 2, rate_decimals: 4 },
    capabilities: {
      espnow: true, economics: true, summary: true, exportCsv: true,
      telemetryConfigPost: true, mqttConfig: true,
      historyTimeRange: true, auth: true, https: true, provisioning: true, configBackup: true,
    },
    endpoints: [
      { path: '/api/v1/schema', methods: 'GET', tier: '2', auth_required: false, requires_reboot: false },
      { path: '/api/v1/espnow', methods: 'GET,POST', tier: '2', auth_required: true, requires_reboot: false },
      { path: '/api/v1/economics', methods: 'GET', tier: '2', params: 'unit', auth_required: false, requires_reboot: false },
      { path: '/api/v1/summary', methods: 'GET', tier: '2', params: 'unit,period,from,to', auth_required: false, requires_reboot: false },
      { path: '/api/v1/export.csv', methods: 'GET', tier: '2', params: 'unit,period,type,from,to', auth_required: false, requires_reboot: false },
      { path: '/api/v1/history', methods: 'GET', tier: '2', params: 'unit_id,period,type,from,to,offset,limit,since', auth_required: false, requires_reboot: false },
      { path: '/api/system/version', methods: 'GET', tier: '1', auth_required: false, requires_reboot: false },
      { path: '/api/system/backup', methods: 'GET,POST', tier: '1', auth_required: true, requires_reboot: false },
      { path: '/api/system/restore', methods: 'POST', tier: '1', auth_required: true, requires_reboot: true },
      { path: '/api/system/ota', methods: 'POST', tier: '1', auth_required: true, requires_reboot: true },
      { path: '/api/system/security', methods: 'GET,POST', tier: '1', auth_required: true, requires_reboot: false },
      { path: '/api/system/provisioning', methods: 'GET', tier: '1', auth_required: false, requires_reboot: false },
      { path: '/api/system/token', methods: 'GET,POST', tier: '1', auth_required: true, requires_reboot: false },
      { path: '/api/system/system-config', methods: 'GET,POST', tier: '1', auth_required: true, requires_reboot: true },
      { path: '/api/system/hardware-config', methods: 'GET,POST', tier: '1', auth_required: true, requires_reboot: true },
      { path: '/api/system/mqtt-config', methods: 'GET,POST', tier: '1', auth_required: true, requires_reboot: true },
      { path: '/api/system/tariff-plan-config', methods: 'GET,POST', tier: '1', auth_required: true, requires_reboot: false },
      { path: '/api/system/units-config', methods: 'GET,POST', tier: '1', auth_required: true, requires_reboot: false },
      { path: '/api/system/telemetry-config', methods: 'GET,POST', tier: '1', auth_required: true, requires_reboot: false },
    ],
  };
}

/** Mock /api/v1/economics (cumulative; money RAW in currency; energy kWh). */
export function mockEconomics(unit = '9020'): Economics {
  return {
    v: 1,
    unit,
    currency: demoRegion().cur,
    // Only the demo plan's two zones carry money — the input's last ~24 h by the site's plan, from
    // the same hourly model as the Overview (UX audit TAR-09/HEU-26: 8 zones with made-up sums before).
    zones: TARIFF_ZONES.map((zone) => {
      const zm = demoZoneMoney();
      return { zone, cost: zone === 'day' ? zm.day.cost : zone === 'night' ? zm.night.cost : 0, revenue: zone === 'day' ? zm.day.rev : zone === 'night' ? zm.night.rev : 0 };
    }),
    // A plan without net metering keeps no kWh credit.
    net_metering: demoTariff().sell == null
      ? { period_import_kwh: 0, period_export_kwh: 0, credit_balance_kwh: 0, lifetime_earned_kwh: 0, lifetime_used_kwh: 0, last_billable_net_kwh: 0 }
      : {
      period_import_kwh: 142.6,
      period_export_kwh: 18.3,
      credit_balance_kwh: 4.2,
      lifetime_earned_kwh: 96.5,
      lifetime_used_kwh: 92.3,
      last_billable_net_kwh: 124.3,
    },
    progressive: { enabled: false, cumulative_import_kwh: 318, tier: 0, rate_mult: 1 }, // the demo plan has none (Тарифы: «выкл»)
  };
}

/** Mock /api/v1/summary (totals windowed, by_zone cumulative; kWh + raw currency). */
export function mockSummary(unit = '9020', period = 'day'): Summary {
  // Windowed totals scale with the window (a month-to-date holds the elapsed days), so the
  // demo's month forecast / comparisons stay plausible against the live «today» (~168 ₽).
  const now = new Date();
  const elapsedMonthDays = now.getDate() - 1 + (now.getHours() * 60 + now.getMinutes()) / 1440;
  const days: Record<string, number> = { hour: 1 / 24, day: 1, week: 7, month: Math.max(elapsedMonthDays, 1 / 24), year: 286 };
  const k = days[period] ?? 1;
  const r = (v: number) => Math.round(v * k * 100) / 100;
  const rm = (v: number) => dm(v * k);
  return {
    v: 1,
    unit,
    period,
    currency: demoRegion().cur,
    totals: {
      records: Math.max(1, Math.round(24 * k)),
      // = the Overview's «today» (31,2 kWh · 168,42 ₽; export 5,2 kWh · 9,36 ₽)
      energy_in_kwh: r(31.2),
      energy_out_kwh: r(5.2),
      net_kwh: r(26),
      cost: rm(168.42),
      revenue: rm(9.36),
      net_cost: rm(159.06),
    },
    by_zone: mockEconomics(unit).zones,
  };
}
