/**
 * Central export file for all RBGrid WebApp constants
 * Import constants from this file for consistent usage across the application
 */

// Namespace imports (must precede the re-exports below — import/first).
import * as EnumConstants from './enums';
import * as SystemConstants from './system';
import * as UIConstants from './ui';
import * as ValidationConstants from './validation';

// Technical enumerations
export * from './enums';

// System constants
export * from './system';

// UI constants
export * from './ui';

// Validation rules
export * from './validation';

// Re-export commonly used constants for convenience
export {
  // Enums
  UNIT_TYPES,
  UNIT_ROLES,
  STAT_CLASSES,
  SENSOR_TYPES,
  TARIFF_PLAN_TYPES,
  TELEMETRY_PROTOCOLS,
  CONNECTION_STATUS,
  TIME_SOURCES,
  WIFI_MODES,
  EVENT_SEVERITY,
} from './enums';

export {
  // System
  API_ENDPOINTS,
  DEFAULT_REFRESH_INTERVALS,
  WEBSOCKET_SETTINGS,
  THRESHOLD_LIMITS,
  SYSTEM_DEFAULTS,
  HTTP_CONFIG,
  FEATURE_FLAGS,
  ENVIRONMENT,
  ERROR_CODES,
} from './system';

export {
  // UI
  CHART_COLORS,
  ICON_MAPPINGS,
  STATUS_COLORS,
  THEME_CONFIG,
  LAYOUT,
  ANIMATIONS,
  BREAKPOINTS,
} from './ui';

export {
  // Validation
  REGEX_PATTERNS,
  FIELD_LENGTHS,
  VALUE_RANGES,
  REQUIRED_FIELDS,
  VALIDATION_MESSAGES,
  VALIDATORS,
} from './validation';

/**
 * Combined constants object for easy access
 */
export const CONSTANTS = {
  // Enums
  ENUMS: {
    UNIT_TYPES: EnumConstants.UNIT_TYPES,
    UNIT_ROLES: EnumConstants.UNIT_ROLES,
    STAT_CLASSES: EnumConstants.STAT_CLASSES,
    SENSOR_TYPES: EnumConstants.SENSOR_TYPES,
    TARIFF_PLAN_TYPES: EnumConstants.TARIFF_PLAN_TYPES,
    TELEMETRY_PROTOCOLS: EnumConstants.TELEMETRY_PROTOCOLS,
    CONNECTION_STATUS: EnumConstants.CONNECTION_STATUS,
    TIME_SOURCES: EnumConstants.TIME_SOURCES,
    WIFI_MODES: EnumConstants.WIFI_MODES,
    EVENT_SEVERITY: EnumConstants.EVENT_SEVERITY,
  },
  
  // System
  SYSTEM: {
    API_ENDPOINTS: SystemConstants.API_ENDPOINTS,
    DEFAULT_REFRESH_INTERVALS: SystemConstants.DEFAULT_REFRESH_INTERVALS,
    WEBSOCKET_SETTINGS: SystemConstants.WEBSOCKET_SETTINGS,
    THRESHOLD_LIMITS: SystemConstants.THRESHOLD_LIMITS,
    SYSTEM_DEFAULTS: SystemConstants.SYSTEM_DEFAULTS,
    HTTP_CONFIG: SystemConstants.HTTP_CONFIG,
    DATA_FORMATS: SystemConstants.DATA_FORMATS,
    PERFORMANCE: SystemConstants.PERFORMANCE,
    FEATURE_FLAGS: SystemConstants.FEATURE_FLAGS,
    ENVIRONMENT: SystemConstants.ENVIRONMENT,
    ERROR_CODES: SystemConstants.ERROR_CODES,
  },
  
  // UI
  UI: {
    CHART_COLORS: UIConstants.CHART_COLORS,
    ICON_MAPPINGS: UIConstants.ICON_MAPPINGS,
    STATUS_COLORS: UIConstants.STATUS_COLORS,
    THEME_CONFIG: UIConstants.THEME_CONFIG,
    LAYOUT: UIConstants.LAYOUT,
    ANIMATIONS: UIConstants.ANIMATIONS,
    Z_INDEX: UIConstants.Z_INDEX,
    BREAKPOINTS: UIConstants.BREAKPOINTS,
    FONTS: UIConstants.FONTS,
    DASHBOARD_WIDGETS: UIConstants.DASHBOARD_WIDGETS,
    NOTIFICATIONS: UIConstants.NOTIFICATIONS,
    TABLE: UIConstants.TABLE,
    FORM: UIConstants.FORM,
  },
  
  // Validation
  VALIDATION: {
    REGEX_PATTERNS: ValidationConstants.REGEX_PATTERNS,
    FIELD_LENGTHS: ValidationConstants.FIELD_LENGTHS,
    VALUE_RANGES: ValidationConstants.VALUE_RANGES,
    REQUIRED_FIELDS: ValidationConstants.REQUIRED_FIELDS,
    VALIDATION_MESSAGES: ValidationConstants.VALIDATION_MESSAGES,
    VALIDATORS: ValidationConstants.VALIDATORS,
    VALIDATION_SCHEMAS: ValidationConstants.VALIDATION_SCHEMAS,
  },
};

/**
 * Helper function to get enum by ID
 */
export const getEnumById = (enumType: string, id: number) => {
  switch (enumType) {
    case 'UNIT_TYPE':
      return Object.values(EnumConstants.UNIT_TYPES).find(item => item.id === id);
    case 'UNIT_ROLE':
      return Object.values(EnumConstants.UNIT_ROLES).find(item => item.id === id);
    case 'STAT_CLASS':
      return Object.values(EnumConstants.STAT_CLASSES).find(item => item.id === id);
    case 'SENSOR_TYPE':
      return Object.values(EnumConstants.SENSOR_TYPES).find(item => item.id === id);
    case 'TARIFF_PLAN_TYPE':
      return Object.values(EnumConstants.TARIFF_PLAN_TYPES).find(item => item.id === id);
    case 'TELEMETRY_PROTOCOL':
      return Object.values(EnumConstants.TELEMETRY_PROTOCOLS).find(item => item.id === id);
    case 'CONNECTION_STATUS':
      return Object.values(EnumConstants.CONNECTION_STATUS).find(item => item.id === id);
    case 'TIME_SOURCE':
      return Object.values(EnumConstants.TIME_SOURCES).find(item => item.id === id);
    case 'WIFI_MODE':
      return Object.values(EnumConstants.WIFI_MODES).find(item => item.id === id);
    case 'EVENT_SEVERITY':
      return Object.values(EnumConstants.EVENT_SEVERITY).find(item => item.id === id);
    default:
      return null;
  }
};

/**
 * Helper function to get enum key by ID
 */
export const getEnumKeyById = (enumType: string, id: number): string | null => {
  const enumObj = getEnumById(enumType, id);
  if (!enumObj) return null;
  
  const enumMap = {
    UNIT_TYPE: EnumConstants.UNIT_TYPES,
    UNIT_ROLE: EnumConstants.UNIT_ROLES,
    STAT_CLASS: EnumConstants.STAT_CLASSES,
    SENSOR_TYPE: EnumConstants.SENSOR_TYPES,
    TARIFF_PLAN_TYPE: EnumConstants.TARIFF_PLAN_TYPES,
    TELEMETRY_PROTOCOL: EnumConstants.TELEMETRY_PROTOCOLS,
    CONNECTION_STATUS: EnumConstants.CONNECTION_STATUS,
    TIME_SOURCE: EnumConstants.TIME_SOURCES,
    WIFI_MODE: EnumConstants.WIFI_MODES,
    EVENT_SEVERITY: EnumConstants.EVENT_SEVERITY,
  }[enumType];
  
  if (!enumMap) return null;
  
  return Object.entries(enumMap).find(([_, value]) => value === enumObj)?.[0] || null;
};

/**
 * Helper function to validate field value
 */
export const validateField = (fieldName: string, value: any, schema: any): string | null => {
  const fieldSchema = schema[fieldName];
  if (!fieldSchema) return null;
  
  // Required validation
  if (fieldSchema.required && !ValidationConstants.VALIDATORS.isRequired(value)) {
    return ValidationConstants.VALIDATION_MESSAGES.REQUIRED;
  }
  
  // Skip other validations if value is empty and not required
  if (!value && !fieldSchema.required) return null;
  
  // Type validation
  if (fieldSchema.type === 'number' && !ValidationConstants.VALIDATORS.isNumeric(value)) {
    return ValidationConstants.VALIDATION_MESSAGES.MUST_BE_NUMBER;
  }
  
  // Length validation
  if (fieldSchema.minLength && !ValidationConstants.VALIDATORS.minLength(value, fieldSchema.minLength)) {
    return ValidationConstants.VALIDATION_MESSAGES.MIN_LENGTH(fieldSchema.minLength);
  }
  
  if (fieldSchema.maxLength && !ValidationConstants.VALIDATORS.maxLength(value, fieldSchema.maxLength)) {
    return ValidationConstants.VALIDATION_MESSAGES.MAX_LENGTH(fieldSchema.maxLength);
  }
  
  // Range validation
  if (fieldSchema.min !== undefined && value < fieldSchema.min) {
    return ValidationConstants.VALIDATION_MESSAGES.MIN_VALUE(fieldSchema.min);
  }
  
  if (fieldSchema.max !== undefined && value > fieldSchema.max) {
    return ValidationConstants.VALIDATION_MESSAGES.MAX_VALUE(fieldSchema.max);
  }
  
  // Pattern validation
  if (fieldSchema.pattern && !fieldSchema.pattern.test(value)) {
    return ValidationConstants.VALIDATION_MESSAGES.INVALID_FORMAT;
  }
  
  // Enum validation
  if (fieldSchema.enum && !fieldSchema.enum.includes(value)) {
    return ValidationConstants.VALIDATION_MESSAGES.REQUIRED_SELECT;
  }
  
  return null;
};

/**
 * Helper function to format measurement value
 */
export const formatMeasurementValue = (value: number, type: string): string => {
  const formats = SystemConstants.DATA_FORMATS.NUMBERS;
  
  switch (type) {
    case 'voltage':
      return value.toFixed(formats.VOLTAGE_DECIMALS) + ' V';
    case 'current':
      return value.toFixed(formats.CURRENT_DECIMALS) + ' A';
    case 'power':
      return value.toFixed(formats.POWER_DECIMALS) + ' W';
    case 'energy':
      return value.toFixed(formats.ENERGY_DECIMALS) + ' kWh';
    case 'cost':
      return value.toFixed(formats.COST_DECIMALS) + ' €';
    case 'percent':
      return value.toFixed(formats.PERCENT_DECIMALS) + ' %';
    default:
      return value.toString();
  }
};

/**
 * Type exports for strict typing
 */
export type { 
  UnitTypeKey,
  UnitRoleKey,
  StatClassKey,
  SensorTypeKey,
  TariffPlanTypeKey,
  TelemetryProtocolKey,
  ConnectionStatusKey,
  TimeSourceKey,
  WiFiModeKey,
  EventSeverityKey,
} from './enums';