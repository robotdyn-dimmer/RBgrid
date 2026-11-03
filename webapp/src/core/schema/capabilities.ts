/**
 * Capability discovery — the schema-driven spine.
 *
 * `GET /api/v1/schema` is a BARE object (NOT BaseResponse-wrapped), confirmed by root
 * 2026-06-23 against the firmware handler:
 *   { api_version:num, telemetry_schema:num, telemetry_profile:"lean"|"full",
 *     fw:str, endpoints:[{path, methods, tier, params?}], transports:{ws,mqtt} }
 * NOTE: endpoint `methods` and `params` are COMMA-SEPARATED STRINGS ("GET,POST"), not arrays.
 *
 * There is no `auth_required`/`capabilities{}` block today — we derive capability flags
 * from the advertised endpoint paths (curated, reliable per root). The optional
 * `capabilities{}` + per-endpoint `auth_required` are a confirmed P0 firmware add; when
 * present they win. Older fw without /schema → BASELINE_CAPABILITIES (graceful degrade).
 */

import { RELEASE_HIDDEN } from '../../constants/release';

export interface RawSchemaEndpoint {
  path: string;
  /** Comma-separated, e.g. "GET,POST". */
  methods?: string;
  tier?: string;
  /** Comma-separated query params, e.g. "unit,from,to". */
  params?: string;
  /** Per-endpoint auth requirement (firmware-landed 2026-06-23). */
  auth_required?: boolean;
  /** Whether POSTing this config endpoint needs a device reboot to converge (landed 2026-06-23). */
  requires_reboot?: boolean;
  /**
   * The fields that apply only at start, on an endpoint mixing live and on-start ones (root,
   * 18.10: system-config, time-sync). Present → it decides per field; requires_reboot then just
   * says «has on-start fields».
   */
  reboot_fields?: string[];
}

export interface RawSchema {
  api_version?: number | string;
  fw?: string;
  /** Live telemetry profile. */
  telemetry_profile?: 'lean' | 'full' | string;
  telemetry_schema?: number | string;
  endpoints?: RawSchemaEndpoint[];
  /** Transport endpoints the device advertises (ws path; broker hint). */
  transports?: { ws?: string; mqtt?: string };
  /** Optional explicit capability flags (confirmed firmware add — preferred when present). */
  capabilities?: Partial<Record<CapabilityFlag, boolean>>;

  // --- contract v4 additive keys (live on canon ≥7e0b1301; confirmed on hub .139 2026-10-12) ---
  /** Build variant: "full" | "oss". In OSS, closed endpoints are absent + answer 405/501. */
  build?: string;
  /** Hardware profile, e.g. "esp32-wrover-psram" / "wrover-16m". */
  hw_profile?: string;
  /** History/stats storage readiness: "ready" | "migrating" | "pending_image_valid" | "incompatible". */
  storage_state?: string;
  storage_progress?: number;
  /**
   * Which record/telemetry fields each source CLASS measures, keyed by the class tag that
   * equals a unit's `src_class` — canon (root 2026-10-12): "rbamp" | "espnow" (no built-in ADC
   * in the product, operator 18.10); "rbamp_std" arrives additively with STD modules. Fields NOT listed
   * are not serialized by that class → the UI shows "—", never "0.00" (U3 / block E.3).
   */
  fields_by_source?: Record<string, string[]>;
  /**
   * Quality-bit legend. Canon = name→bit ({"PARTIAL":0,…}, root 2026-10-12). `normalizeQualityBits`
   * also accepts the older bit→name draft shape for safety.
   */
  quality_bits?: Record<string, number | string>;
  /** Time-quality snapshot (RC §5 placed it in schema; live device carries it in telemetry). */
  time_quality?: { tq?: number; time_src?: string; sync_age_s?: number };

  // --- configurable surface (canon fc0f962b, firmware note configurable-surface 2026-10-13) ---
  /** Money display: active tariff plan currency (ISO 4217) + ISO minor-unit decimals; rates 4. */
  money?: { currency_code?: string; money_decimals?: number; rate_decimals?: number };
  /** Wire units. energy is ALWAYS float Wh on the wire (mWh is internal v4 storage only). */
  units_of_measure?: Record<string, string>;
  /** unit_key "HEX4" = %04X UPPERCASE; legacy_supply_keys → telemetry still keys supplies "main"/"alt". */
  key_format?: { unit_key?: string; legacy_supply_keys?: boolean };
  /** Field limits (canon f451b6a5): byte limits are UTF-8 bytes, not characters. */
  limits?: { short_name_bytes?: number; name_bytes?: number; origins_max?: number; ranges?: Record<string, HubRange> };
  /** Firmware enums (role, phase, site_profile, tariff_*). Fixed by firmware, read — never hardcode. */
  enums?: {
    role?: string[];
    site_profile?: string[];
    phase?: Record<string, string>;
    /** CT models a current channel can be set to (root dd861a54): {"4": "SCT-013-050", …}. */
    ct_model?: Record<string, string>;
    /** CT- series factory sensors, shown not chosen (62f1c9a2). */
    ct_model_ct?: Record<string, string>;
    tariff_zone?: string[];
    tariff_plan_type?: string[];
    tariff_condition_type?: string[];
    /** UI languages the hub's system.language accepts (installer choice; absent = not a setting yet). */
    language?: string[];
    /** Mains systems system.grid.system accepts (root delta №4): 1P / SPLIT / 3P. */
    grid_system?: string[];
    /** Mains systems validated on hardware (root F20); absent = only 1P. Others are labelled «не проверено». */
    grid_system_validated?: string[];
  };
}

/** Money formatting parameters (from schema.money; defaults until the device says otherwise). */
export interface MoneyFormat {
  /** ISO 4217 code of the active tariff plan, or null when unknown (no currency shown). */
  currencyCode: string | null;
  /** Decimals for sums (ISO minor units: 0 JPY…, 2 most, 3 KWD/BHD…). */
  moneyDecimals: number;
  /** Decimals for per-kWh rates. */
  rateDecimals: number;
}

export const DEFAULT_MONEY_FORMAT: MoneyFormat = { currencyCode: null, moneyDecimals: 2, rateDecimals: 4 };

export type CapabilityFlag =
  | 'espnow'
  | 'economics'
  | 'summary'
  | 'exportCsv'
  | 'telemetryConfigPost'
  | 'mqttConfig'
  | 'historyTimeRange'
  | 'auth'
  | 'https'
  | 'provisioning'
  | 'configBackup'
  | 'schema';

export type TelemetryProfile = 'lean' | 'full' | 'unknown';

export interface DeviceCapabilities {
  apiVersion: string | null;
  fwVersion: string | null;
  telemetryProfile: TelemetryProfile;
  /** True when derived from a real schema (vs the baseline fallback). */
  fromSchema: boolean;
  flags: Record<CapabilityFlag, boolean>;
  endpoints: RawSchemaEndpoint[];
  transports: { ws?: string; mqtt?: string };
  // --- contract v4 additive ---
  /** "full" | "oss" | null. OSS hides closed sections (don't trust schema for tariff — known mismatch). */
  build: string | null;
  hwProfile: string | null;
  storageState: string | null;
  /** source tag → measured field names (for dash-not-zero gating). Empty when the device omits it. */
  fieldsBySource: Record<string, string[]>;
  /** Canonical bit→name quality legend (normalized from either schema shape). */
  qualityBits: Record<number, string>;
  // --- configurable surface (canon fc0f962b) ---
  money: MoneyFormat;
  /** Firmware role enum (empty when the device doesn't publish it). */
  roleEnum: string[];
  /** schema.enums.language — present only when system.language is a real installer setting. */
  languageEnum: string[];
  /** Every string-list enum in schema.enums by field name (generic editors render them as selects). */
  stringEnums: Record<string, string[]>;
  /** schema.enums.ct_model: code → model name («SCT-013-050»); empty when the hub doesn't offer it. */
  ctModels: Record<number, string>;
  /** schema.enums.ct_model_ct: CT- series names (fixed, shown only). */
  ctModelsFixed: Record<number, string>;
  /** Telemetry still keys supply units "main"/"alt" (and the bus "v_bus") instead of HEX4. */
  legacySupplyKeys: boolean;
  /** Field limits from schema.limits; defaults = the oldest firmware that lacks the block. */
  limits: DeviceLimits;
}

/** One numeric setting's range as the hub validates it (canon 2d2653ee, rbjson.c s_cfg_ranges):
 *  `section` = config domain, `path` = dotted path inside it in the POST body; `zero_unset` = 0
 *  is allowed as «not set»; `read_only` = a mirror the POST ignores. */
export interface HubRange { min?: number; max?: number; decimals?: number; section?: string; path?: string; zero_unset?: boolean; read_only?: boolean }

/** UTF-8 byte limits / list sizes the hub validates (schema.limits, canon f451b6a5). */
export interface DeviceLimits {
  shortNameBytes: number; nameBytes: number; originsMax: number;
  /** Numeric field ranges by LEAF config key (schema.limits.ranges, canon 2d2653ee). */
  ranges?: Record<string, HubRange>;
}
/** Pre-f451b6a5 hubs: short_name 10 B, name 31 B, CORS origins 8 (c400dd12). */
export const DEFAULT_LIMITS: DeviceLimits = { shortNameBytes: 10, nameBytes: 31, originsMax: 8 };

const posInt = (v: unknown, d: number): number => (typeof v === 'number' && Number.isInteger(v) && v > 0 ? v : d);
function limitsOf(schema: RawSchema): DeviceLimits {
  const l = schema.limits ?? {};
  return {
    shortNameBytes: posInt(l.short_name_bytes, DEFAULT_LIMITS.shortNameBytes),
    nameBytes: posInt(l.name_bytes, DEFAULT_LIMITS.nameBytes),
    originsMax: posInt(l.origins_max, DEFAULT_LIMITS.originsMax),
    ...(l.ranges && typeof l.ranges === 'object' ? { ranges: l.ranges } : {}),
  };
}

const ALL_FLAGS: CapabilityFlag[] = [
  'espnow', 'economics', 'summary', 'exportCsv', 'telemetryConfigPost',
  'mqttConfig', 'historyTimeRange', 'auth', 'https',
  'provisioning', 'configBackup', 'schema',
];

/** The pre-schema baseline: none of the new surfaces, telemetry-config GET-only. */
export const BASELINE_CAPABILITIES: DeviceCapabilities = {
  apiVersion: null,
  fwVersion: null,
  telemetryProfile: 'unknown',
  fromSchema: false,
  endpoints: [],
  transports: {},
  flags: ALL_FLAGS.reduce((acc, f) => ({ ...acc, [f]: false }), {} as Record<CapabilityFlag, boolean>),
  build: null,
  hwProfile: null,
  storageState: null,
  fieldsBySource: {},
  qualityBits: {},
  money: DEFAULT_MONEY_FORMAT,
  roleEnum: [],
  languageEnum: [],
  stringEnums: {},
  ctModels: {},
  ctModelsFixed: {},
  legacySupplyKeys: true,
  limits: DEFAULT_LIMITS,
};

const intOr = (v: unknown, d: number): number =>
  typeof v === 'number' && Number.isInteger(v) && v >= 0 && v <= 6 ? v : d;

function moneyFormatOf(schema: RawSchema): MoneyFormat {
  const m = schema.money ?? {};
  const code = typeof m.currency_code === 'string' && /^[A-Z]{3}$/.test(m.currency_code) ? m.currency_code : null;
  return {
    currencyCode: code,
    moneyDecimals: intOr(m.money_decimals, DEFAULT_MONEY_FORMAT.moneyDecimals),
    rateDecimals: intOr(m.rate_decimals, DEFAULT_MONEY_FORMAT.rateDecimals),
  };
}

/**
 * Normalize the schema's `quality_bits` into a canonical bit→name map, accepting either
 * the live hub shape (name→bit: {"PARTIAL":0}) or the RC §5 draft (bit→name: {"0":"PARTIAL"}).
 * Heuristic: if the values are numbers, it's name→bit; if the keys are numeric, bit→name.
 */
export function normalizeQualityBits(qb: Record<string, number | string> | undefined): Record<number, string> {
  const out: Record<number, string> = {};
  if (!qb) return out;
  for (const [k, v] of Object.entries(qb)) {
    if (typeof v === 'number') out[v] = k;                 // name→bit (live)
    else if (/^\d+$/.test(k)) out[Number(k)] = String(v);  // bit→name (RC draft)
  }
  return out;
}

const csv = (s: string | undefined): string[] =>
  (s ?? '').split(',').map((x) => x.trim()).filter(Boolean);

const methodsOf = (e: RawSchemaEndpoint): string[] => {
  const m = csv(e.methods);
  return m.length ? m.map((x) => x.toUpperCase()) : ['GET'];
};

const paramsOf = (e: RawSchemaEndpoint): string[] => csv(e.params);

const has = (eps: RawSchemaEndpoint[], path: string, method = 'GET'): boolean =>
  eps.some((e) => e.path === path && methodsOf(e).includes(method.toUpperCase()));

/** Derive capabilities from a schema (or null → baseline). Explicit flags win. */
export function deriveCapabilities(schema: RawSchema | null): DeviceCapabilities {
  if (!schema) return BASELINE_CAPABILITIES;

  const eps = schema.endpoints ?? [];
  const explicit = schema.capabilities ?? {};
  const flag = (f: CapabilityFlag, inferred: boolean): boolean =>
    explicit[f] !== undefined ? Boolean(explicit[f]) : inferred;

  const profile: TelemetryProfile =
    schema.telemetry_profile === 'lean' || schema.telemetry_profile === 'full'
      ? schema.telemetry_profile
      : 'unknown';

  const authInferred = eps.some((e) => e.auth_required === true);
  const historyTimeRange = eps.some(
    (e) => e.path === '/api/v1/history' && paramsOf(e).includes('from'),
  );

  return {
    apiVersion: schema.api_version != null ? String(schema.api_version) : null,
    fwVersion: schema.fw ?? null,
    telemetryProfile: profile,
    fromSchema: true,
    endpoints: eps,
    transports: schema.transports ?? {},
    build: schema.build ?? null,
    hwProfile: schema.hw_profile ?? null,
    storageState: schema.storage_state ?? null,
    fieldsBySource: schema.fields_by_source ?? {},
    qualityBits: normalizeQualityBits(schema.quality_bits),
    money: moneyFormatOf(schema),
    roleEnum: Array.isArray(schema.enums?.role) ? schema.enums!.role!.filter((r) => typeof r === 'string') : [],
    languageEnum: Array.isArray(schema.enums?.language) ? schema.enums!.language!.filter((r) => typeof r === 'string') : [],
    ctModels: codeMap(schema.enums?.ct_model),
    ctModelsFixed: codeMap(schema.enums?.ct_model_ct),
    stringEnums: Object.fromEntries(Object.entries(schema.enums ?? {})
      .filter(([, v]) => Array.isArray(v) && v.every((x) => typeof x === 'string')) as [string, string[]][]),
    // Absent key_format = pre-fc0f962b firmware, which also emitted "main"/"alt".
    legacySupplyKeys: schema.key_format?.legacy_supply_keys !== false,
    limits: limitsOf(schema),
    flags: {
      schema: true,
      // Not in the v1.1.0 release: the code stays, the UI is hidden in a release build.
      espnow: !RELEASE_HIDDEN.espnow && flag('espnow', has(eps, '/api/v1/espnow')),
      economics: flag('economics', has(eps, '/api/v1/economics')),
      summary: flag('summary', has(eps, '/api/v1/summary')),
      exportCsv: flag('exportCsv', has(eps, '/api/v1/export.csv')),
      telemetryConfigPost: flag('telemetryConfigPost', has(eps, '/api/system/telemetry-config', 'POST')),
      mqttConfig: flag('mqttConfig', has(eps, '/api/system/mqtt-config')),
      historyTimeRange: flag('historyTimeRange', historyTimeRange),
      auth: flag('auth', authInferred),
      https: flag('https', false), // transport-level; explicit only
      provisioning: flag('provisioning', has(eps, '/api/system/provisioning')),
      configBackup: flag('configBackup', has(eps, '/api/system/backup')),
    },
  };
}

/** Config domain → its config endpoint path (for per-endpoint schema lookups). */
export const DOMAIN_ENDPOINT: Record<string, string> = {
  system: '/api/system/system-config',
  hardware: '/api/system/hardware-config',
  time: '/api/system/time-config',
  tariff: '/api/system/tariff-plan-config',
  units: '/api/system/units-config',
  mqtt: '/api/system/mqtt-config',
  telemetry: '/api/system/telemetry-config',
  wifi: '/api/system/wifi-config',
  espnow: '/api/v1/espnow',
};

/**
 * Device-driven reboot flag for a config domain, from the schema's per-endpoint
 * `requires_reboot` (landed 2026-06-23). Returns undefined when the schema doesn't
 * cover it (caller falls back to the static REBOOT_DOMAINS heuristic).
 */
export function domainRequiresReboot(caps: DeviceCapabilities, domain: string): boolean | undefined {
  const path = DOMAIN_ENDPOINT[domain];
  if (!path) return undefined;
  const ep = caps.endpoints.find((e) => e.path === path);
  return ep?.requires_reboot;
}

/** A config domain's on-start field list from the schema (undefined: the schema doesn't say). */
export function domainRebootFields(caps: DeviceCapabilities, domain: string): string[] | undefined {
  const path = DOMAIN_ENDPOINT[domain];
  return path ? endpointRebootFields(caps, path) : undefined;
}

/** The endpoint's on-start field list, when the schema gives one (see RawSchemaEndpoint). */
export function endpointRebootFields(caps: DeviceCapabilities, path: string): string[] | undefined {
  const f = caps.endpoints.find((e) => e.path === path)?.reboot_fields;
  return Array.isArray(f) ? f : undefined;
}

/** {"4": "SCT-013-050"} → {4: "SCT-013-050"} (non-integer keys / non-string names dropped). */
function codeMap(o: Record<string, string> | undefined): Record<number, string> {
  return Object.fromEntries(Object.entries(o ?? {})
    .filter(([k, v]) => Number.isInteger(Number(k)) && typeof v === 'string').map(([k, v]) => [Number(k), v]));
}

/** Is a capability available? */
export function can(caps: DeviceCapabilities, flag: CapabilityFlag): boolean {
  return caps.flags[flag] === true;
}

/** Whether a given endpoint+method requires auth (per the schema, when it says so). */
export function endpointRequiresAuth(caps: DeviceCapabilities, path: string): boolean {
  return caps.endpoints.some((e) => e.path === path && e.auth_required === true);
}

/** True for the OSS build variant (closed sections absent; answer 405/501). */
export function isOssBuild(caps: DeviceCapabilities): boolean {
  return (caps.build ?? '').toLowerCase() === 'oss';
}

/** True while the device is migrating its stats store (history returns 503 + Retry-After). */
export function isStorageMigrating(caps: DeviceCapabilities): boolean {
  return caps.storageState === 'migrating';
}

/**
 * The field names a source actually measures, per schema.fields_by_source. Returns null
 * when the schema omits the source entirely → caller must NOT gate (show all, don't blank),
 * because "unknown" differs from "measured set is empty". (U3 / block E.3 / DoD §9 #6.)
 */
export function fieldsForSource(caps: DeviceCapabilities, src: string | undefined): string[] | null {
  if (!src) return null;
  const list = caps.fieldsBySource[src];
  return Array.isArray(list) ? list : null;
}

/**
 * Should the UI render `field` for a value from `src`? True when the schema doesn't describe
 * the source (unknown → don't hide), or when the field is in that source's measured set.
 * False only when we positively know the source omits the field → render "—", never "0.00".
 */
export function isFieldMeasured(caps: DeviceCapabilities, src: string | undefined, field: string): boolean {
  const list = fieldsForSource(caps, src);
  return list === null ? true : list.includes(field);
}
