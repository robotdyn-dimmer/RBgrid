/**
 * Device and measurement unit types for RBGrid WebApp
 * These types define the structure of devices, sensors, and measurement units
 */

/**
 * Measurement unit types enumeration
 */
export type UnitType = 'VOLTAGE_BUS' | 'CURRENT_LOAD' | 'POWER_METER';

/**
 * Unit role in the power system
 */
/**
 * Unit role — the firmware enum (schema `enums.role`, canon fc0f962b): 10 UPPER strings.
 * Generation = RENEWABLE_SOURCE (ALTERNATIVE_SUPPLY is a supply, e.g. a generator/second
 * input). There is no "GENERATION" role. Single source: constants/enums UNIT_ROLE_NAMES.
 */
export type UnitRole = import('../constants/enums').UnitRoleName;

/**
 * Statistics aggregation class
 */
export type StatClass = '1HOUR' | 'DAILY' | 'MONTHLY' | 'YEARLY';

/**
 * Data source types
 */
/** units-config `source_type` as the hub writes it. Legacy hubs still say «ADC_SENSOR» on units
 *  that are rbAmp channels (no built-in ADC in the product) — not shown, not branched on. */
export type SourceType = string;

/**
 * Sensor types enumeration
 */
export type SensorType = 
  | 'ZMPT101B'      // Voltage transformer
  | 'SCT013'        // Current transformer
  | 'ACS712'        // Hall effect current sensor
  | 'PZEM004T'      // Power meter module
  | 'CUSTOM'        // Custom sensor
  | 'VIRTUAL';      // Calculated/virtual sensor

/**
 * Data source configuration
 */
export interface Source {
  /** Source type identifier */
  source_type: SourceType;
  /** Source ID */
  source_id: number;
  /** Associated sensor profile ID */
  sensor_profile_id: number;
  /** Weight factor for aggregation (0.0 - 1.0) */
  weight_factor: number;
  /** Primary source flag */
  is_primary: boolean;
  /** Invert measurement sign */
  invert_sign: boolean;
}

/**
 * Threshold configuration for alerts
 */
export interface Thresholds {
  /** Over power threshold in watts */
  over_power_w: number;
  /** Under voltage threshold in volts */
  under_voltage_v: number;
  /** Over current threshold in amperes */
  over_current_a: number;
  /** Over voltage threshold in volts */
  over_voltage_v: number;
  /** Enable threshold alerts */
  enable_alerts: boolean;
}

/**
 * Billing configuration
 */
export interface Billing {
  /** Cost per kilowatt-hour */
  cost_per_kwh: number;
  /** Enable time-of-use pricing */
  time_of_use_enabled: boolean;
  /** Current active tariff zone */
  current_tariff_zone: number;
}

/**
 * Main measurement unit interface
 */
export interface Unit {
  /** Unique unit identifier */
  unit_id: number;
  /** Display name */
  name: string;
  /** Physical location */
  location: string;
  /** Unit measurement type */
  type: UnitType;
  /** Unit role in the system */
  role: UnitRole;
  /** Statistics aggregation class */
  stat_class: StatClass;
  /** Data sources array */
  sources: Source[];
  /** Number of active sources */
  sources_count: number;
  /** Parent unit ID for hierarchical structure */
  parent_unit_id: number;
  /** Child unit IDs */
  child_units: number[];
  /** Number of child units */
  children_count: number;
  /** Support for bidirectional power flow */
  bidirectional_metering: boolean;
  /** Accumulate energy readings */
  accumulate_energy: boolean;
  /** Rated power in watts */
  rated_power_w: number;
  /** Maximum current in amperes */
  max_current_a: number;
  /** Rated voltage in volts */
  rated_voltage_v: number;
  /** Alert thresholds */
  thresholds: Thresholds;
  /** Billing configuration */
  billing: Billing;
  /** Unit active status */
  is_active: boolean;
  /** Invert power sign for generation units */
  is_inverted?: boolean;
  /** Creation timestamp in microseconds */
  created_timestamp_us: number;
  /** Has real-time data available */
  has_realtime_data: boolean;
  /** Statistics collection enabled */
  has_statistics_enabled: boolean;
  /** Last update timestamp in microseconds */
  last_update_us: number;
  /** Total updates received */
  total_updates: number;

  // ---- contract v4 / T7 additive (live ≥7e0b1301) — opaque identity + point ----
  /** Stable point slug ([a-z0-9_.-]{1,32}); editable only via the rename op. */
  point?: string;
  /** Capability bitmask: 1=V 2=I 4=P 8=E 16=bidi 32=PF 64=freq 128=remote 256=virtual. */
  caps?: number;
  /** Opaque physical identity of the bound sensor (survives rebind). */
  device_uid?: string | null;
  /** Channel index within the bound device. */
  channel_idx?: number;
  // ---- UI-design gaps G2/G5/G6 — root-confirmed 2026-10-12 (land next canon; keep fallbacks) ----
  /** Short label (≤10 chars) for EnergyFlowTree. "" = unset → UI generates from `name` (G2). */
  short_name?: string;
  /**
   * Electrical phase (G6), NUMERIC: 0=unset, 1..3=L1..L3, 4=3P. POST-validated 0..4.
   * UI maps the number to the L1/L2/L3/3ф label. Persisted for I2C units.
   */
  phase?: number;
  /** Line group id (G5, phase-2 semantics): 0 = none. Reserved field, type final. */
  group_id?: number;
  /** Trunk role within a group (G5, phase-2 semantics): 0/1. Reserved field, type final. */
  role_trunk?: number;
}

/**
 * Units configuration data
 */
export interface UnitsConfigData {
  /** Array of configured units */
  units: Unit[];
  /** Total units count */
  units_count: number;
  /** Active units count */
  active_units_count: number;
  /** System health status */
  system_healthy: boolean;
  /** v4/T7: slugs of removed points — a bind to one returns 409 POINT_TOMBSTONED (reuse:true revives). */
  point_tombstones?: string[];
}
