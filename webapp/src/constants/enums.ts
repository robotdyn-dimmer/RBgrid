/**
 * Technical enumerations and static constants for RBGrid WebApp
 * Based on hybrid strategy for optimal performance with ESP32
 */

/**
 * Unit types with numeric IDs for efficient data transfer
 */
export const UNIT_TYPES = {
  VOLTAGE_BUS: { 
    id: 0, 
    name: 'Voltage Bus', 
    icon: 'zap', 
    color: '#f59e0b',
    description: 'Voltage measurement unit'
  },
  CURRENT_LOAD: { 
    id: 1, 
    name: 'Current Load', 
    icon: 'activity', 
    color: '#3b82f6',
    description: 'Current measurement and load monitoring'
  },
  POWER_METER: { 
    id: 2, 
    name: 'Power Meter', 
    icon: 'gauge', 
    color: '#10b981',
    description: 'Complete power measurement unit'
  },
  ENERGY_STORAGE: { 
    id: 3, 
    name: 'Energy Storage', 
    icon: 'battery', 
    color: '#8b5cf6',
    description: 'Battery or energy storage system'
  },
  SOLAR_PANEL: { 
    id: 4, 
    name: 'Solar Panel', 
    icon: 'sun', 
    color: '#f59e0b',
    description: 'Solar generation unit'
  },
  WIND_TURBINE: { 
    id: 5, 
    name: 'Wind Turbine', 
    icon: 'wind', 
    color: '#06b6d4',
    description: 'Wind generation unit'
  }
} as const;

/**
 * Unit roles in the power system
 */
export const UNIT_ROLES = {
  MAIN_SUPPLY: { 
    id: 0, 
    name: 'Main Supply', 
    priority: 1, 
    color: '#dc2626',
    description: 'Primary power supply from grid'
  },
  ALTERNATIVE_SUPPLY: { 
    id: 1, 
    name: 'Alternative Supply', 
    priority: 2, 
    color: '#059669',
    description: 'Alternative/backup power supply'
  },
  ZONE_LOAD: { 
    id: 2, 
    name: 'Zone Load', 
    priority: 3, 
    color: '#3b82f6',
    description: 'Specific zone or area load'
  },
  GENERATION: { 
    id: 3, 
    name: 'Generation', 
    priority: 4, 
    color: '#f59e0b',
    description: 'Power generation unit'
  },
  STORAGE: { 
    id: 4, 
    name: 'Storage', 
    priority: 5, 
    color: '#8b5cf6',
    description: 'Energy storage system'
  }
} as const;

/**
 * Statistics aggregation classes
 */
export const STAT_CLASSES = {
  REALTIME: { 
    id: 0, 
    name: 'Real-time', 
    interval_ms: 1000, 
    retention_hours: 1,
    description: 'Live data streaming'
  },
  '1MINUTE': { 
    id: 1, 
    name: '1 Minute', 
    interval_ms: 60000, 
    retention_hours: 24,
    description: 'Minute aggregation'
  },
  '5MINUTE': { 
    id: 2, 
    name: '5 Minutes', 
    interval_ms: 300000, 
    retention_hours: 48,
    description: '5-minute aggregation'
  },
  '1HOUR': { 
    id: 3, 
    name: '1 Hour', 
    interval_ms: 3600000, 
    retention_days: 30,
    description: 'Hourly aggregation'
  },
  DAILY: { 
    id: 4, 
    name: 'Daily', 
    interval_ms: 86400000, 
    retention_days: 365,
    description: 'Daily aggregation'
  },
  MONTHLY: { 
    id: 5, 
    name: 'Monthly', 
    interval_ms: 2592000000, 
    retention_years: 10,
    description: 'Monthly aggregation'
  },
  YEARLY: { 
    id: 6, 
    name: 'Yearly', 
    interval_ms: 31536000000, 
    retention_years: 50,
    description: 'Yearly aggregation'
  }
} as const;

/**
 * Sensor types with specifications
 */
export const SENSOR_TYPES = {
  VOLTAGE_ZMPT107_1: { 
    id: 0, 
    name: 'ZMPT107-1 (Voltage)', 
    max_value: 250, 
    unit: 'V',
    accuracy: 0.5,
    type: 'voltage'
  },
  CURRENT_SCT013_10A: { 
    id: 1, 
    name: 'SCT013-10A (Current)', 
    max_value: 10, 
    unit: 'A',
    accuracy: 1,
    type: 'current'
  },
  CURRENT_SCT013_50A: { 
    id: 2, 
    name: 'SCT013-50A (Current)', 
    max_value: 50, 
    unit: 'A',
    accuracy: 1,
    type: 'current'
  },
  CURRENT_SCT013_100A: { 
    id: 3, 
    name: 'SCT013-100A (Current)', 
    max_value: 100, 
    unit: 'A',
    accuracy: 1,
    type: 'current'
  },
  POWER_PZEM004T: { 
    id: 4, 
    name: 'PZEM-004T (Power)', 
    max_value: 100000, 
    unit: 'W',
    accuracy: 0.5,
    type: 'power'
  },
  VOLTAGE_CUSTOM: { 
    id: 5, 
    name: 'Custom Voltage Sensor', 
    max_value: 500, 
    unit: 'V',
    accuracy: 2,
    type: 'voltage'
  },
  CURRENT_CUSTOM: { 
    id: 6, 
    name: 'Custom Current Sensor', 
    max_value: 200, 
    unit: 'A',
    accuracy: 2,
    type: 'current'
  }
} as const;

/**
 * Tariff plan types
 */
export const TARIFF_PLAN_TYPES = {
  FLAT: { 
    id: 0, 
    name: 'Flat Rate', 
    zones: 1,
    description: 'Single flat rate for all times'
  },
  TWO_ZONE: { 
    id: 1, 
    name: 'Two Zone', 
    zones: 2,
    description: 'Day and night rates'
  },
  THREE_ZONE: { 
    id: 2, 
    name: 'Three Zone', 
    zones: 3,
    description: 'Peak, standard, and off-peak rates'
  },
  PROGRESSIVE: { 
    id: 3, 
    name: 'Progressive', 
    zones: 0,
    description: 'Rate increases with usage'
  }
} as const;

/**
 * Telemetry protocols
 */
export const TELEMETRY_PROTOCOLS = {
  MQTT: { 
    id: 0, 
    name: 'MQTT', 
    default_port: 1883,
    secure_port: 8883,
    description: 'MQTT protocol for IoT'
  },
  HTTP: { 
    id: 1, 
    name: 'HTTP', 
    default_port: 80,
    secure_port: 443,
    description: 'HTTP REST API'
  },
  WEBSOCKET: { 
    id: 2, 
    name: 'WebSocket', 
    default_port: 8080,
    secure_port: 8443,
    description: 'WebSocket for real-time data'
  },
  MODBUS: { 
    id: 3, 
    name: 'Modbus', 
    default_port: 502,
    secure_port: 802,
    description: 'Modbus TCP protocol'
  }
} as const;

/**
 * Connection status
 */
export const CONNECTION_STATUS = {
  CONNECTED: { 
    id: 0, 
    name: 'Connected', 
    color: '#10b981',
    icon: 'check-circle'
  },
  DISCONNECTED: { 
    id: 1, 
    name: 'Disconnected', 
    color: '#ef4444',
    icon: 'x-circle'
  },
  CONNECTING: { 
    id: 2, 
    name: 'Connecting', 
    color: '#f59e0b',
    icon: 'loader'
  },
  ERROR: { 
    id: 3, 
    name: 'Error', 
    color: '#dc2626',
    icon: 'alert-circle'
  }
} as const;

/**
 * Time sources for synchronization
 */
export const TIME_SOURCES = {
  SNTP: { 
    id: 0, 
    name: 'SNTP', 
    priority: 1,
    description: 'Network Time Protocol'
  },
  MANUAL: { 
    id: 1, 
    name: 'Manual', 
    priority: 3,
    description: 'Manual time setting'
  },
  ESP_NOW: { 
    id: 2, 
    name: 'ESP-NOW', 
    priority: 2,
    description: 'ESP-NOW time sync'
  },
  RTC: { 
    id: 3, 
    name: 'RTC', 
    priority: 4,
    description: 'Real-time clock'
  },
  CPU: { 
    id: 4, 
    name: 'CPU', 
    priority: 5,
    description: 'CPU internal timer'
  }
} as const;

/**
 * WiFi modes
 */
export const WIFI_MODES = {
  AP: { 
    id: 0, 
    name: 'Access Point', 
    description: 'Device acts as WiFi hotspot'
  },
  STA: { 
    id: 1, 
    name: 'Station', 
    description: 'Device connects to WiFi network'
  },
  APSTA: { 
    id: 2, 
    name: 'AP + Station', 
    description: 'Both AP and Station modes'
  }
} as const;

/**
 * WiFi authentication / encryption modes.
 * Static mirror of esp_wifi WIFI_AUTH_* (firmware GET /wifi/scan/results auth_mode).
 * Keys match the WiFiAuthMode string union 1:1. 'unknown' = firmware fallback.
 */
export const WIFI_AUTH_MODES = {
  open:            { name: 'Open',            secure: false, esp: 'WIFI_AUTH_OPEN' },
  wep:             { name: 'WEP',             secure: false, esp: 'WIFI_AUTH_WEP' },
  wpa_psk:         { name: 'WPA',             secure: true,  esp: 'WIFI_AUTH_WPA_PSK' },
  wpa2_psk:        { name: 'WPA2',            secure: true,  esp: 'WIFI_AUTH_WPA2_PSK' },
  wpa_wpa2_psk:    { name: 'WPA/WPA2',        secure: true,  esp: 'WIFI_AUTH_WPA_WPA2_PSK' },
  wpa3_psk:        { name: 'WPA3',            secure: true,  esp: 'WIFI_AUTH_WPA3_PSK' },
  wpa2_wpa3_psk:   { name: 'WPA2/WPA3',       secure: true,  esp: 'WIFI_AUTH_WPA2_WPA3_PSK' },
  wpa2_enterprise: { name: 'WPA2-Enterprise', secure: true,  esp: 'WIFI_AUTH_WPA2_ENTERPRISE' },
  unknown:         { name: 'Unknown',         secure: false, esp: '(fallback)' }
} as const;

export type WiFiAuthModeKey = keyof typeof WIFI_AUTH_MODES;
export const WIFI_AUTH_MODE_VALUES = Object.keys(WIFI_AUTH_MODES) as WiFiAuthModeKey[];

/**
 * Event severity levels
 */
export const EVENT_SEVERITY = {
  INFO: { 
    id: 0, 
    name: 'Info', 
    color: '#3b82f6',
    icon: 'info'
  },
  WARNING: { 
    id: 1, 
    name: 'Warning', 
    color: '#f59e0b',
    icon: 'alert-triangle'
  },
  ERROR: { 
    id: 2, 
    name: 'Error', 
    color: '#ef4444',
    icon: 'alert-circle'
  },
  CRITICAL: { 
    id: 3, 
    name: 'Critical', 
    color: '#dc2626',
    icon: 'alert-octagon'
  }
} as const;

/**
 * Utility functions for enum lookups
 */
export const getUnitTypeById = (id: number) => {
  return Object.values(UNIT_TYPES).find(type => type.id === id);
};

export const getUnitRoleById = (id: number) => {
  return Object.values(UNIT_ROLES).find(role => role.id === id);
};

export const getStatClassById = (id: number) => {
  return Object.values(STAT_CLASSES).find(stat => stat.id === id);
};

export const getSensorTypeById = (id: number) => {
  return Object.values(SENSOR_TYPES).find(sensor => sensor.id === id);
};

export const getTariffPlanTypeById = (id: number) => {
  return Object.values(TARIFF_PLAN_TYPES).find(type => type.id === id);
};

export const getTelemetryProtocolById = (id: number) => {
  return Object.values(TELEMETRY_PROTOCOLS).find(protocol => protocol.id === id);
};

/**
 * Type extractions for TypeScript
 */
export type UnitTypeKey = keyof typeof UNIT_TYPES;
export type UnitRoleKey = keyof typeof UNIT_ROLES;
export type StatClassKey = keyof typeof STAT_CLASSES;
export type SensorTypeKey = keyof typeof SENSOR_TYPES;
export type TariffPlanTypeKey = keyof typeof TARIFF_PLAN_TYPES;
export type TelemetryProtocolKey = keyof typeof TELEMETRY_PROTOCOLS;
export type ConnectionStatusKey = keyof typeof CONNECTION_STATUS;
export type TimeSourceKey = keyof typeof TIME_SOURCES;
export type WiFiModeKey = keyof typeof WIFI_MODES;
export type EventSeverityKey = keyof typeof EVENT_SEVERITY;

/**
 * Enum values for validation
 */
export const UNIT_TYPE_VALUES = Object.keys(UNIT_TYPES) as UnitTypeKey[];
export const UNIT_ROLE_VALUES = Object.keys(UNIT_ROLES) as UnitRoleKey[];
export const STAT_CLASS_VALUES = Object.keys(STAT_CLASSES) as StatClassKey[];

/**
 * Firmware-exact unit role / stat-class strings (rbpower_load_role_t /
 * rbpower_stat_period_type_t), confirmed by the firmware team 2026-06-19 for the
 * POST /api/v1/units bind contract. Case-sensitive, exact match.
 *
 * NOTE: the older `UNIT_ROLES`/`STAT_CLASSES` objects above and `device.ts` UnitRole/
 * StatClass unions are DIVERGENT/incomplete (UnitRole has only 4 + a fictional
 * 'GENERATION'; StatClass misses 3SEC/15MIN/WEEKLY). Realign device.ts to these during
 * the devices-UI phase (cascades into Record<UnitRole,…> + mocks — deferred).
 */
export const UNIT_ROLE_NAMES = [
  'MAIN_SUPPLY', 'ALTERNATIVE_SUPPLY', 'CIRCUIT_BREAKER', 'DEVICE_LOAD', 'ZONE_LOAD',
  'RENEWABLE_SOURCE', 'STORAGE_SYSTEM', 'EV_CHARGING', 'HEATING_COOLING', 'SMART_APPLIANCE'
] as const;
export type UnitRoleName = typeof UNIT_ROLE_NAMES[number];

export const STAT_CLASS_NAMES = [
  '3SEC', '15MIN', '1HOUR', 'DAILY', 'WEEKLY', 'MONTHLY', 'YEARLY'
] as const;
export type StatClassName = typeof STAT_CLASS_NAMES[number];

/**
 * Stat-classes practical to pick for a unit. 3SEC/15MIN are RAM-only internal periods —
 * not valid as a unit's persisted stat_class (root). Use for the bind/settings dropdown.
 */
export const UNIT_STAT_CLASS_CHOICES: StatClassName[] = ['1HOUR', 'DAILY', 'WEEKLY', 'MONTHLY', 'YEARLY'];

/**
 * Firmware-exact tariff enums (rbpower_tariff_*), confirmed by root 2026-06-20.
 * Case-sensitive, exact match — for the Tariffs form dropdowns.
 */
export const TARIFF_PLAN_TYPE_NAMES = [
  'SIMPLE', 'TWO_ZONE', 'THREE_ZONE', 'MULTI_ZONE', 'PROGRESSIVE', 'SEASONAL', 'DYNAMIC', 'CUSTOM',
] as const;
export type TariffPlanType = typeof TARIFF_PLAN_TYPE_NAMES[number];

export const TARIFF_ZONE_ID_NAMES = [
  'NIGHT', 'MORNING_PEAK', 'DAY', 'EVENING_PEAK', 'LATE_EVENING', 'WEEKEND', 'HOLIDAY', 'CRITICAL_PEAK',
] as const;
export type TariffZoneId = typeof TARIFF_ZONE_ID_NAMES[number];

export const TARIFF_CONDITION_TYPE_NAMES = [
  'TIME_RANGE', 'WEEKDAY', 'WEEKEND', 'HOLIDAY', 'SEASON', 'MONTH', 'DATE_RANGE', 'SPECIAL_EVENT',
] as const;
export type TariffConditionType = typeof TARIFF_CONDITION_TYPE_NAMES[number];
export const SENSOR_TYPE_VALUES = Object.keys(SENSOR_TYPES) as SensorTypeKey[];
export const TARIFF_PLAN_TYPE_VALUES = Object.keys(TARIFF_PLAN_TYPES) as TariffPlanTypeKey[];
export const TELEMETRY_PROTOCOL_VALUES = Object.keys(TELEMETRY_PROTOCOLS) as TelemetryProtocolKey[];
export const CONNECTION_STATUS_VALUES = Object.keys(CONNECTION_STATUS) as ConnectionStatusKey[];
export const TIME_SOURCE_VALUES = Object.keys(TIME_SOURCES) as TimeSourceKey[];
export const WIFI_MODE_VALUES = Object.keys(WIFI_MODES) as WiFiModeKey[];
export const EVENT_SEVERITY_VALUES = Object.keys(EVENT_SEVERITY) as EventSeverityKey[];