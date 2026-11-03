/**
 * System constants for RBGrid WebApp
 * API endpoints, default settings, and system configurations
 */

/**
 * API endpoint configuration
 */
export const API_ENDPOINTS = {
  // Config domains — authoritative paths per DEVICE_API_SURFACE.md (GET reads, POST persists).
  CONFIG: {
    SYSTEM: '/api/system/system-config',
    HARDWARE: '/api/system/hardware-config',
    TIME: '/api/system/time-config',
    UNITS: '/api/system/units-config',
    TARIFFS: '/api/system/tariff-plan-config',
    TELEMETRY: '/api/system/telemetry-config',
    WIFI: '/api/system/wifi-config'
  },

  // WiFi endpoints (firmware-served, no /api prefix).
  WIFI: {
    SCAN: '/wifi/scan',
    SCAN_RESULTS: '/wifi/scan/results',
    STATUS: '/wifi/status',
    SAVE: '/wifi/save'
  },

  // Device/system admin (per DEVICE_API_SURFACE.md).
  DEVICE: {
    REBOOT: '/api/system/reboot',
    RESTORE: '/api/system/restore-system-config',
    /** v1.1.2: a real factory reset (root 01d14d6) — {history:"erase"|"keep"}, the hub restarts as new. */
    FACTORY_RESET: '/api/system/factory-reset',
    TIME_SYNC: '/api/system/time-sync',
    // @deprecated not in DEVICE_API_SURFACE.md — device status is derivable from telemetry;
    // calibrate endpoint TBD. Kept so existing (unwired) helpers compile.
    STATUS: '/api/device/status',
    CALIBRATE: '/api/device/calibrate',
  },
  
  // Statistics history — FINAL contract: single endpoint, flat (non-envelope) response.
  // Mirrors firmware GET /api/v1/history?unit_id&period&type&offset&limit. See types/history.ts.
  HISTORY: '/api/v1/history',

  // Phase 4 config-lifecycle (v1 resource paths — distinct from CONFIG.UNITS = /api/config/units).
  // GET /api/v1/sensors (BaseResponse) · DELETE /api/v1/units/{hexid} (flat). See types/sensors.ts.
  SENSORS: '/api/v1/sensors',
  /** /api/v1/modules/<m>/firmware — module update through the hub (root 534ded9e). */
  MODULES: '/api/v1/modules',
  /** /api/v1/i2c — rbAmp fleet on the I2C bus: scan, add, readdress (rbi2c_api.c, fw ≥ 1.1.0). */
  I2C: '/api/v1/i2c',
  UNITS: '/api/v1/units',
  // Live telemetry snapshot (compact frame, same shape as the /ws-telemetry push) —
  // for the dashboard's first frame before the WS connects. See types/telemetry-units.ts.
  TELEMETRY_LATEST: '/api/telemetry/latest',

  // @deprecated Diverges from firmware (which serves the single /api/v1/history above).
  // Realtime is WS-only — there is NO /api/statistics/realtime. Slated for removal with the
  // stats-cleanup once charts consume getHistory().
  STATISTICS: {
    HOURLY: '/api/statistics/hourly',
    DAILY: '/api/statistics/daily',
    MONTHLY: '/api/statistics/monthly',
    YEARLY: '/api/statistics/yearly'
  },
  
  // WebSocket endpoints — device serves the live telemetry channel at /ws-telemetry
  // (authoritative per DEVICE_API_SURFACE.md; compact frame, 3s push).
  WEBSOCKET: {
    TELEMETRY: '/ws-telemetry',
    EVENTS: '/ws/events',
    STATUS: '/ws/status'
  }
} as const;

/**
 * Default refresh intervals (in seconds)
 */
export const DEFAULT_REFRESH_INTERVALS = {
  TELEMETRY: 1,           // Real-time data
  DASHBOARD: 5,           // Dashboard updates
  STATISTICS: 30,         // Statistics refresh
  CONFIGURATION: 300,     // Configuration sync
  DEVICE_STATUS: 10,      // Device status check
  NETWORK_STATUS: 15,     // Network connectivity check
} as const;

/**
 * WebSocket settings — read by services/websocket.ts (mapped explicitly to its camelCase
 * options; B-1: they used to be spread under the wrong keys and every value was undefined).
 */
export const WEBSOCKET_SETTINGS = {
  // Reconnection parameters — exponential backoff 1 s → ×2 → capped at 30 s, ±30 % jitter.
  RECONNECT: {
    INITIAL_DELAY: 1000,      // Initial reconnect delay (ms)
    MAX_DELAY: 30000,         // Maximum reconnect delay (ms)
    MULTIPLIER: 2,            // Backoff multiplier
    MAX_ATTEMPTS: 0,          // 0 = unlimited (a hub that reboots/drops off WiFi must come back on its own; the 30 s cap bounds the load)
    JITTER: 0.3,              // Random jitter factor (0-1), ± fraction of the delay
  },

  // Timeout settings
  TIMEOUTS: {
    CONNECTION: 10000,        // Connect (open) timeout (ms) — a socket stuck CONNECTING is dropped and retried
    // Liveness: the firmware pushes a frame every 3 s and does NOT answer an app-level 'ping'
    // (rbwebsocket_telemetry.c answers only protocol PING frames, which browsers can't send),
    // so the client sends no ping — it watches inbound silence instead.
    SILENCE: 15000,           // No inbound message for this long → socket considered dead → reconnect (ms)
    SILENCE_CHECK: 5000,      // How often the silence watchdog runs (ms)
  },

  // Outgoing buffer (messages sent while disconnected — flushed on open, oldest dropped)
  BUFFER: {
    MAX_SIZE: 100,           // Maximum buffered messages
  }
} as const;

/**
 * Threshold limits for measurements
 */
export const THRESHOLD_LIMITS = {
  VOLTAGE: {
    MIN: 0,                   // Minimum voltage (V)
    MAX: 500,                 // Maximum voltage (V)
    // Warning band = nominal ±10 % from the hub (entities/gridSystem voltageBand), not fixed volts.
  },
  
  CURRENT: {
    MIN: 0,                   // Minimum current (A)
    MAX: 200,                 // Maximum current (A)
    WARNING_PERCENT: 80,      // Warning at % of max current
    CRITICAL_PERCENT: 95,     // Critical at % of max current
  },
  
  POWER: {
    MIN: 0,                   // Minimum power (W)
    MAX: 100000,              // Maximum power (W)
    WARNING_PERCENT: 85,      // Warning at % of rated power
    CRITICAL_PERCENT: 100,    // Critical at % of rated power
  },
  
  FREQUENCY: {
    MIN: 45,                  // Minimum frequency (Hz)
    MAX: 65,                  // Maximum frequency (Hz)
    NOMINAL: 50,              // Nominal frequency (Hz)
    TOLERANCE: 0.5,           // Frequency tolerance (Hz)
  },
  
  TEMPERATURE: {
    MIN: -40,                 // Minimum temperature (°C)
    MAX: 125,                 // Maximum temperature (°C)
    WARNING: 70,              // Warning temperature (°C)
    CRITICAL: 85,             // Critical temperature (°C)
  },
  
  POWER_FACTOR: {
    MIN: 0,                   // Minimum power factor
    MAX: 1,                   // Maximum power factor
    WARNING: 0.85,            // Warning threshold
    CRITICAL: 0.7,            // Critical threshold
  }
} as const;

/**
 * System default values
 */
export const SYSTEM_DEFAULTS = {
  // Device settings
  DEVICE: {
    NAME: 'RBGrid Device',
    LOCATION: 'Not Set',
    LANGUAGE: 'EN',
    TIMEZONE: 'UTC+0',
  },
  
  // Network settings
  NETWORK: {
    /** The hub's own AP is `rbgrid-<mac4>` (last 4 hex of the STA MAC = the mDNS host, root 20.10). */
    AP_SSID_PREFIX: 'rbgrid-',
    AP_CHANNEL: 1,
    AP_MAX_CONNECTIONS: 4,
    SNTP_SERVER: 'pool.ntp.org',
    SNTP_BACKUP: 'time.nist.gov',
    SNTP_TIMEOUT: 10000,
  },
  
  // Measurement settings
  MEASUREMENT: {
    SAMPLING_FREQUENCY: 20000,    // Hz
    AVERAGING_SAMPLES: 10,        // Number of samples
    CALIBRATION_DURATION: 60,     // Seconds
    NOISE_THRESHOLD: 0.5,         // Percent
  },
  
  // Storage settings
  STORAGE: {
    MAX_MEMORY_USAGE: 80,         // Percent
    LOG_RETENTION_DAYS: 30,       // Days
    STATISTICS_RETENTION: {
      HOURLY: 48,                 // Hours
      DAILY: 365,                 // Days
      MONTHLY: 120,               // Months
    }
  }
} as const;

/**
 * HTTP request configuration
 */
export const HTTP_CONFIG = {
  // Timeout settings (ms)
  TIMEOUTS: {
    DEFAULT: 30000,
    CONFIG_READ: 10000,
    CONFIG_WRITE: 15000,
    STATISTICS: 20000,
    FILE_UPLOAD: 60000,
    DEVICE_COMMAND: 5000,
  },
  
  // Retry configuration
  RETRY: {
    MAX_ATTEMPTS: 3,
    INITIAL_DELAY: 1000,
    MAX_DELAY: 5000,
    BACKOFF_FACTOR: 2,
  },
  
  // Request headers — keep ONLY simple headers so cross-origin GETs don't trigger a CORS
  // preflight (Content-Type:application/json and custom X-* are non-simple). axios re-adds
  // Content-Type automatically for POST/PUT bodies (those still preflight — device handles it).
  HEADERS: {
    'Accept': 'application/json',
  }
} as const;

/**
 * Data format settings
 */
export const DATA_FORMATS = {
  // Number formatting
  NUMBERS: {
    VOLTAGE_DECIMALS: 1,
    CURRENT_DECIMALS: 2,
    POWER_DECIMALS: 0,
    ENERGY_DECIMALS: 3,
    COST_DECIMALS: 2,
    PERCENT_DECIMALS: 1,
  },
  
  // Date/time formats
  DATETIME: {
    DATE_FORMAT: 'YYYY-MM-DD',
    TIME_FORMAT: 'HH:mm:ss',
    DATETIME_FORMAT: 'YYYY-MM-DD HH:mm:ss',
    TIMESTAMP_FORMAT: 'x',        // Unix timestamp in ms
  },
  
  // Export formats
  EXPORT: {
    CSV_SEPARATOR: ',',
    CSV_LINE_ENDING: '\n',
    JSON_INDENT: 2,
    EXCEL_SHEET_NAME: 'RBGrid Data',
  }
} as const;

/**
 * Performance settings
 */
export const PERFORMANCE = {
  // Debounce delays (ms)
  DEBOUNCE: {
    INPUT: 300,
    SEARCH: 500,
    RESIZE: 100,
    SCROLL: 50,
  },
  
  // Batch processing
  BATCH: {
    SIZE: 50,
    DELAY: 100,
  },
  
  // Cache settings
  CACHE: {
    MAX_AGE: 300000,              // 5 minutes
    MAX_ITEMS: 1000,
    STALE_WHILE_REVALIDATE: 60000, // 1 minute
  },
  
  // Rendering
  RENDER: {
    FPS_TARGET: 60,
    CHART_UPDATE_INTERVAL: 1000,
    TABLE_VIRTUAL_SCROLL_BUFFER: 5,
  }
} as const;

/**
 * Feature flags
 */
export const FEATURE_FLAGS = {
  ENABLE_WEBSOCKET: true,
  ENABLE_OFFLINE_MODE: false,
  ENABLE_EXPORT: true,
  ENABLE_NOTIFICATIONS: true,
  ENABLE_DARK_MODE: true,
  ENABLE_MULTI_LANGUAGE: true,
  ENABLE_ADVANCED_CHARTS: true,
  ENABLE_DATA_LOGGING: true,
  ENABLE_REMOTE_CONFIG: true,
  ENABLE_BETA_FEATURES: false,
} as const;

/**
 * Environment detection
 */
export const ENVIRONMENT = {
  IS_PRODUCTION: process.env.NODE_ENV === 'production',
  IS_DEVELOPMENT: process.env.NODE_ENV === 'development',
  IS_TEST: process.env.NODE_ENV === 'test',
  // Production build is served BY the device → same-origin (relative ''); requests hit
  // whatever host:port served the page. Dev / remote use REACT_APP_API_BASE_URL (device IP).
  API_BASE_URL: process.env.REACT_APP_API_BASE_URL
    || (process.env.NODE_ENV === 'production' ? '' : 'http://192.168.1.100'),
  WS_PORT: parseInt(process.env.REACT_APP_WS_PORT || '8080', 10),
  VERSION: process.env.REACT_APP_VERSION || '2.0.0',
  USE_MOCK_DATA: process.env.REACT_APP_USE_MOCK_DATA === 'true' || process.env.NODE_ENV === 'development',
} as const;

/**
 * Error codes
 */
export const ERROR_CODES = {
  // Network errors
  NETWORK_ERROR: 'E001',
  TIMEOUT: 'E002',
  CONNECTION_LOST: 'E003',
  
  // API errors
  INVALID_RESPONSE: 'E100',
  AUTHENTICATION_FAILED: 'E101',
  PERMISSION_DENIED: 'E102',
  RESOURCE_NOT_FOUND: 'E103',
  
  // Device errors
  DEVICE_OFFLINE: 'E200',
  DEVICE_ERROR: 'E201',
  CALIBRATION_FAILED: 'E202',
  
  // Data errors
  INVALID_DATA: 'E300',
  DATA_CORRUPTION: 'E301',
  STORAGE_FULL: 'E302',
  
  // Application errors
  UNKNOWN_ERROR: 'E999',
} as const;