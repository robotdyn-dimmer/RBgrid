/**
 * Validation rules and constraints for RBGrid WebApp
 * Input validation, regex patterns, and form constraints
 */

/**
 * Regular expression patterns
 */
export const REGEX_PATTERNS = {
  // Network patterns
  IP_ADDRESS: /^(?:(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.){3}(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)$/,
  MAC_ADDRESS: /^([0-9A-Fa-f]{2}[:-]){5}([0-9A-Fa-f]{2})$/,
  URL: /^https?:\/\/(www\.)?[-a-zA-Z0-9@:%._\+~#=]{1,256}\.[a-zA-Z0-9()]{1,6}\b([-a-zA-Z0-9()@:%_\+.~#?&//=]*)$/,
  HOSTNAME: /^[a-zA-Z0-9]([a-zA-Z0-9\-]{0,61}[a-zA-Z0-9])?(\.[a-zA-Z0-9]([a-zA-Z0-9\-]{0,61}[a-zA-Z0-9])?)*$/,
  PORT: /^([0-9]{1,4}|[1-5][0-9]{4}|6[0-4][0-9]{3}|65[0-4][0-9]{2}|655[0-2][0-9]|6553[0-5])$/,
  
  // Device patterns
  DEVICE_ID: /^[a-zA-Z0-9_-]{3,32}$/,
  DEVICE_NAME: /^[a-zA-Z0-9\s_-]{1,64}$/,
  LOCATION: /^[a-zA-Z0-9\s,._-]{1,128}$/,
  
  // WiFi patterns
  SSID: /^[a-zA-Z0-9!@#$%^&*()_+\-=\[\]{};':"\\|,.<>\/?`~\s]{1,32}$/,
  WPA_KEY: /^[\x20-\x7E]{8,63}$/,
  
  // General patterns
  ALPHANUMERIC: /^[a-zA-Z0-9]+$/,
  NUMERIC: /^[0-9]+$/,
  DECIMAL: /^[0-9]+(\.[0-9]+)?$/,
  EMAIL: /^[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,}$/,
  PHONE: /^[\+]?[(]?[0-9]{3}[)]?[-\s\.]?[(]?[0-9]{3}[)]?[-\s\.]?[0-9]{4,6}$/,
  
  // Time patterns
  TIME_24H: /^([01]?[0-9]|2[0-3]):[0-5][0-9]$/,
  TIMEZONE: /^UTC[+-]([0-9]|1[0-2])$/,
  
  // Currency patterns
  CURRENCY_CODE: /^[A-Z]{3}$/,
  PRICE: /^\d+(\.\d{1,2})?$/,
} as const;

/**
 * Field length constraints
 */
export const FIELD_LENGTHS = {
  // System configuration
  DEVICE_ID: { min: 3, max: 32 },
  DEVICE_NAME: { min: 1, max: 64 },
  HOME_NAME: { min: 1, max: 64 },
  LOCATION: { min: 1, max: 128 },
  LANGUAGE_CODE: { min: 2, max: 5 },
  
  // Network configuration
  SSID: { min: 1, max: 32 },
  PASSWORD: { min: 8, max: 63 },
  IP_ADDRESS: { min: 7, max: 15 },
  MAC_ADDRESS: { min: 17, max: 17 },
  HOSTNAME: { min: 1, max: 253 },
  
  // Unit configuration
  UNIT_NAME: { min: 1, max: 64 },
  UNIT_LOCATION: { min: 1, max: 64 },
  
  // Tariff configuration
  PLAN_NAME: { min: 1, max: 64 },
  PROVIDER_NAME: { min: 1, max: 64 },
  DESCRIPTION: { min: 0, max: 256 },
  ZONE_NAME: { min: 1, max: 32 },
  ZONE_ID: { min: 1, max: 16 },
  
  // User input
  SEARCH_QUERY: { min: 1, max: 100 },
  NOTES: { min: 0, max: 1000 },
  API_TOKEN: { min: 16, max: 128 },
} as const;

/**
 * Numeric value ranges
 */
export const VALUE_RANGES = {
  // Voltage ranges (V)
  VOLTAGE: {
    MIN: 0,
    MAX: 500,
    RATED_MIN: 100,
    RATED_MAX: 480,
    THRESHOLD_MIN: 0,
    THRESHOLD_MAX: 600,
  },
  
  // Current ranges (A)
  CURRENT: {
    MIN: 0,
    MAX: 200,
    SENSOR_10A: { MIN: 0, MAX: 10 },
    SENSOR_50A: { MIN: 0, MAX: 50 },
    SENSOR_100A: { MIN: 0, MAX: 100 },
    THRESHOLD_MIN: 0,
    THRESHOLD_MAX: 250,
  },
  
  // Power ranges (W)
  POWER: {
    MIN: 0,
    MAX: 100000,
    RATED_MIN: 0,
    RATED_MAX: 50000,
    THRESHOLD_MIN: 0,
    THRESHOLD_MAX: 100000,
  },
  
  // Frequency ranges (Hz)
  FREQUENCY: {
    MIN: 45,
    MAX: 65,
    NOMINAL_50HZ: { MIN: 49.5, MAX: 50.5 },
    NOMINAL_60HZ: { MIN: 59.5, MAX: 60.5 },
  },
  
  // Temperature ranges (°C)
  TEMPERATURE: {
    MIN: -40,
    MAX: 125,
    OPERATING_MIN: -20,
    OPERATING_MAX: 85,
    WARNING: 70,
    CRITICAL: 85,
  },
  
  // Power factor ranges
  POWER_FACTOR: {
    MIN: 0,
    MAX: 1,
    WARNING_MIN: 0.85,
    CRITICAL_MIN: 0.7,
  },
  
  // Percentage ranges
  PERCENTAGE: {
    MIN: 0,
    MAX: 100,
  },
  
  // Time ranges
  TIME: {
    HOUR: { MIN: 0, MAX: 23 },
    MINUTE: { MIN: 0, MAX: 59 },
    SECOND: { MIN: 0, MAX: 59 },
    INTERVAL_MS: { MIN: 100, MAX: 3600000 },
    TIMEOUT_MS: { MIN: 100, MAX: 60000 },
  },
  
  // Network ranges
  NETWORK: {
    PORT: { MIN: 1, MAX: 65535 },
    CHANNEL: { MIN: 1, MAX: 14 },
    RSSI: { MIN: -100, MAX: 0 },
    MAX_CONNECTIONS: { MIN: 1, MAX: 10 },
  },
  
  // Billing ranges
  BILLING: {
    RATE: { MIN: 0, MAX: 10000 },
    VAT: { MIN: 0, MAX: 100 },
    CREDIT: { MIN: 0, MAX: 1000000 },
  }
} as const;

/**
 * Required fields for each configuration type.
 *
 * ⚠ LEGACY form-level hints, NOT the device's config shapes (e.g. the device's system-config is
 * `{system:{…}}` with every field optional; time-config is `{config,statistics}` and GET-only).
 * Do not use these to gate config writes — see services/utils.ts validateConfigData (B-2).
 */
export const REQUIRED_FIELDS = {
  // System configuration
  SYSTEM: ['home_name', 'location', 'language'],
  
  // Hardware configuration
  HARDWARE: ['channels', 'sampling_frequency'],
  
  // Time configuration
  TIME: ['enable_time_sync', 'primary_source', 'timezone'],
  
  // Unit configuration
  UNIT: ['name', 'location', 'type', 'role', 'rated_voltage_v'],
  
  // Tariff configuration
  TARIFF: ['plan_name', 'provider_name', 'plan_type', 'currency_code'],
  
  // Tariff zone configuration
  TARIFF_ZONE: ['zone_id', 'zone_name', 'rate_buying_per_kwh'],
  
  // Telemetry configuration
  TELEMETRY: ['telemetry_enabled', 'json_buffer_size'],
  
  // WiFi configuration
  WIFI_AP: ['ssid', 'channel', 'max_connection'],
  WIFI_STA: ['ssid', 'password'],
  
  // Sensor configuration
  SENSOR: ['enabled', 'type', 'name'],
  
  // Source configuration
  SOURCE: ['source_type', 'source_id', 'weight_factor'],
} as const;

/**
 * Validation messages
 */
export const VALIDATION_MESSAGES = {
  // Required field messages
  REQUIRED: 'This field is required',
  REQUIRED_SELECT: 'Please select an option',
  
  // Length messages
  MIN_LENGTH: (min: number) => `Minimum ${min} characters required`,
  MAX_LENGTH: (max: number) => `Maximum ${max} characters allowed`,
  EXACT_LENGTH: (length: number) => `Must be exactly ${length} characters`,
  
  // Range messages
  MIN_VALUE: (min: number) => `Minimum value is ${min}`,
  MAX_VALUE: (max: number) => `Maximum value is ${max}`,
  RANGE: (min: number, max: number) => `Value must be between ${min} and ${max}`,
  
  // Format messages
  INVALID_FORMAT: 'Invalid format',
  INVALID_IP: 'Invalid IP address format',
  INVALID_MAC: 'Invalid MAC address format',
  INVALID_EMAIL: 'Invalid email format',
  INVALID_URL: 'Invalid URL format',
  INVALID_SSID: 'Invalid SSID format',
  INVALID_DEVICE_ID: 'Device ID must be 3-32 characters, alphanumeric with - and _',
  
  // Type messages
  MUST_BE_NUMBER: 'Must be a number',
  MUST_BE_INTEGER: 'Must be a whole number',
  MUST_BE_POSITIVE: 'Must be a positive number',
  
  // Network messages
  PORT_RANGE: 'Port must be between 1 and 65535',
  CHANNEL_RANGE: 'Channel must be between 1 and 14',
  
  // Password messages
  PASSWORD_TOO_SHORT: 'Password must be at least 8 characters',
  PASSWORD_TOO_LONG: 'Password must be less than 64 characters',
  PASSWORD_STRENGTH: 'Password is too weak',
  
  // Confirmation messages
  PASSWORDS_DONT_MATCH: 'Passwords do not match',
  VALUES_DONT_MATCH: 'Values do not match',
  
  // Custom messages
  THRESHOLD_LOGIC: 'Warning threshold must be less than critical threshold',
  TIME_RANGE_LOGIC: 'End time must be after start time',
  PARENT_CHILD_CONFLICT: 'A unit cannot be its own parent',
  DUPLICATE_ENTRY: 'This value already exists',
} as const;

/**
 * Validation functions
 */
export const VALIDATORS = {
  // Basic validators
  isRequired: (value: any) => value !== null && value !== undefined && value !== '',
  isEmail: (value: string) => REGEX_PATTERNS.EMAIL.test(value),
  isNumeric: (value: any) => !isNaN(parseFloat(value)) && isFinite(value),
  isInteger: (value: any) => Number.isInteger(Number(value)),
  isPositive: (value: number) => value > 0,
  isInRange: (value: number, min: number, max: number) => value >= min && value <= max,
  
  // Network validators
  isIpAddress: (value: string) => REGEX_PATTERNS.IP_ADDRESS.test(value),
  isMacAddress: (value: string) => REGEX_PATTERNS.MAC_ADDRESS.test(value),
  isPort: (value: any) => {
    const port = parseInt(value, 10);
    return !isNaN(port) && port >= 1 && port <= 65535;
  },
  isUrl: (value: string) => REGEX_PATTERNS.URL.test(value),
  isHostname: (value: string) => REGEX_PATTERNS.HOSTNAME.test(value),
  
  // String validators
  minLength: (value: string, min: number) => value.length >= min,
  maxLength: (value: string, max: number) => value.length <= max,
  lengthBetween: (value: string, min: number, max: number) => 
    value.length >= min && value.length <= max,
  
  // Device validators
  isDeviceId: (value: string) => REGEX_PATTERNS.DEVICE_ID.test(value),
  isDeviceName: (value: string) => REGEX_PATTERNS.DEVICE_NAME.test(value),
  isSsid: (value: string) => REGEX_PATTERNS.SSID.test(value),
  
  // Time validators
  isTime24h: (value: string) => REGEX_PATTERNS.TIME_24H.test(value),
  isTimezone: (value: string) => REGEX_PATTERNS.TIMEZONE.test(value),
  
  // Custom validators
  isValidThresholds: (warning: number, critical: number) => warning < critical,
  isValidTimeRange: (start: string, end: string) => {
    const [startHour, startMin] = start.split(':').map(Number);
    const [endHour, endMin] = end.split(':').map(Number);
    const startMinutes = startHour * 60 + startMin;
    const endMinutes = endHour * 60 + endMin;
    return startMinutes < endMinutes;
  },
  isValidParentChild: (unitId: number, parentId: number) => unitId !== parentId,
} as const;

/**
 * Form validation schemas
 */
export const VALIDATION_SCHEMAS = {
  // System configuration schema
  SYSTEM_CONFIG: {
    home_name: {
      required: true,
      minLength: FIELD_LENGTHS.HOME_NAME.min,
      maxLength: FIELD_LENGTHS.HOME_NAME.max,
    },
    location: {
      required: true,
      minLength: FIELD_LENGTHS.LOCATION.min,
      maxLength: FIELD_LENGTHS.LOCATION.max,
    },
    language: {
      required: true,
      pattern: /^[A-Z]{2}$/,
    },
  },
  
  // Unit configuration schema
  UNIT_CONFIG: {
    name: {
      required: true,
      minLength: FIELD_LENGTHS.UNIT_NAME.min,
      maxLength: FIELD_LENGTHS.UNIT_NAME.max,
    },
    type: {
      required: true,
      enum: ['VOLTAGE_BUS', 'CURRENT_LOAD', 'POWER_METER'],
    },
    role: {
      required: true,
      enum: ['MAIN_SUPPLY', 'ALTERNATIVE_SUPPLY', 'ZONE_LOAD', 'GENERATION'],
    },
    rated_voltage_v: {
      required: true,
      type: 'number',
      min: VALUE_RANGES.VOLTAGE.RATED_MIN,
      max: VALUE_RANGES.VOLTAGE.RATED_MAX,
    },
    max_current_a: {
      type: 'number',
      min: VALUE_RANGES.CURRENT.MIN,
      max: VALUE_RANGES.CURRENT.MAX,
    },
  },
  
  // WiFi configuration schema
  WIFI_CONFIG: {
    ap: {
      ssid: {
        required: true,
        pattern: REGEX_PATTERNS.SSID,
        minLength: FIELD_LENGTHS.SSID.min,
        maxLength: FIELD_LENGTHS.SSID.max,
      },
      channel: {
        required: true,
        type: 'number',
        min: VALUE_RANGES.NETWORK.CHANNEL.MIN,
        max: VALUE_RANGES.NETWORK.CHANNEL.MAX,
      },
    },
    sta: {
      ssid: {
        required: true,
        pattern: REGEX_PATTERNS.SSID,
        minLength: FIELD_LENGTHS.SSID.min,
        maxLength: FIELD_LENGTHS.SSID.max,
      },
      password: {
        required: true,
        minLength: FIELD_LENGTHS.PASSWORD.min,
        maxLength: FIELD_LENGTHS.PASSWORD.max,
      },
    },
  },
} as const;