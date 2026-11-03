/**
 * Utility functions for RBGrid WebApp services
 * Helper functions for data formatting, validation, and calculations
 */

import { AxiosResponse } from 'axios';
import {
  BaseResponse,
  APIError,
  ConfigType,
  HardwareConfigData,
  TimeConfigData,
  TelemetryConfigData,
  WiFiConfigData,
} from '../types';
import {
  UnitsConfigData,
  Unit,
} from '../types/device';
import {
  TariffConfigData,
} from '../types/tariff';
import {
  ERROR_CODES,
  VALIDATION_SCHEMAS,
  VALIDATORS,
  VALIDATION_MESSAGES,
  DATA_FORMATS,
  VALUE_RANGES,
} from '../constants';

/**
 * Power calculation data
 */
export interface PowerData {
  /** Active power in watts */
  activePower: number;
  /** Reactive power in VAR */
  reactivePower: number;
  /** Apparent power in VA */
  apparentPower: number;
  /** Power factor (0-1) */
  powerFactor: number;
  /** Phase angle in degrees */
  phaseAngle: number;
}

/**
 * Format API response to ensure consistent structure
 */
export function formatApiResponse<T>(response: AxiosResponse<BaseResponse<T>>): BaseResponse<T> {
  const data = response.data;
  
  // Validate base response structure
  if (!data || typeof data !== 'object') {
    throw new Error('Invalid API response format');
  }

  // Ensure all required fields are present
  const formattedResponse: BaseResponse<T> = {
    type: data.type || 'unknown',
    version: data.version || '2.0',
    device_id: data.device_id || 'unknown',
    timestamp_local: data.timestamp_local || 0,
    timestamp_ntp: data.timestamp_ntp || Date.now() * 1000,
    data: data.data,
  };

  // Validate data field
  if (formattedResponse.data === undefined || formattedResponse.data === null) {
    throw new Error('API response missing data field');
  }

  return formattedResponse;
}

/**
 * Light structural sanity check of a config doc against the REAL device shapes.
 *
 * ⚠ Not a write gate: the config write path (DirectTransport.setConfig / api.update*) no
 * longer calls this — the device is the authority and answers structured 400s. The old
 * version demanded legacy keys (`system_settings`, top-level `home_name/location/language`,
 * `enable_time_sync/primary_source/timezone`) that the firmware never sends, so every real
 * doc was rejected before the network (B-2). Accepts the inner object OR the POST envelope.
 */
export function validateConfigData(type: ConfigType, data: any): boolean {
  if (!data || typeof data !== 'object' || Array.isArray(data)) {
    return false;
  }

  switch (type) {
    case 'system':
      return validateSystemConfig(data);
    case 'hardware':
      return validateHardwareConfig(data.data ?? data);
    case 'time':
      return validateTimeConfig(data.data ?? data);
    case 'units':
      return validateUnitsConfig(data.data ?? data);
    case 'tariffs':
      return validateTariffsConfig(data.data ?? data);
    case 'telemetry':
      return validateTelemetryConfig(data.data ?? data);
    case 'wifi':
      return validateWiFiConfig(data.data ?? data);
    default:
      return false;
  }
}

/**
 * Validate system configuration — `{system:{…}}` envelope or the inner object; all fields
 * optional (the device keeps absent ones). Legacy `system_settings` is still tolerated.
 */
function validateSystemConfig(data: Record<string, any>): boolean {
  const settings = (data.system ?? data.system_settings ?? data) as Record<string, any>;
  if (!settings || typeof settings !== 'object') return false;

  // Validate home name
  if (settings.home_name !== undefined) {
    if (!VALIDATORS.lengthBetween(settings.home_name, 1, 64)) {
      return false;
    }
  }

  // Validate location
  if (settings.location !== undefined) {
    if (!VALIDATORS.lengthBetween(settings.location, 1, 128)) {
      return false;
    }
  }

  // Validate language
  if (settings.language !== undefined) {
    if (!/^[A-Z]{2}$/.test(settings.language)) {
      return false;
    }
  }

  // G1: site_profile enum (schema enums.site_profile)
  if (settings.site_profile !== undefined) {
    if (!['home', 'industrial', 'ev'].includes(settings.site_profile)) {
      return false;
    }
  }

  return true;
}

/**
 * Validate hardware configuration (grid nominals only; 0 = not set).
 */
function validateHardwareConfig(data: Partial<HardwareConfigData>): boolean {
  const n = data.network;
  if (!n) return false;
  if (n.nominal_frequency !== undefined && n.nominal_frequency !== 0 && !VALIDATORS.isInRange(n.nominal_frequency, 45, 65)) return false;
  if (n.nominal_voltage !== undefined && n.nominal_voltage !== 0 && !VALIDATORS.isInRange(n.nominal_voltage, 90, 280)) return false;
  return true;
}

/**
 * Validate time configuration
 */
function validateTimeConfig(data: Partial<TimeConfigData>): boolean {
  const config = (data.config ?? {}) as NonNullable<Partial<TimeConfigData>['config']>;
  
  // Validate primary source
  if (config.primary_source !== undefined) {
    if (!['SNTP', 'MANUAL', 'ESP_NOW'].includes(config.primary_source)) {
      return false;
    }
  }
  
  // Validate timezone
  if (config.timezone !== undefined) {
    if (!/^UTC[+-]\d{1,2}$/.test(config.timezone)) {
      return false;
    }
  }
  
  return true;
}

/**
 * Validate units configuration
 */
function validateUnitsConfig(data: Partial<UnitsConfigData>): boolean {
  if (!data.units || !Array.isArray(data.units)) {
    return false;
  }
  
  // Validate each unit
  for (const unit of data.units) {
    if (!validateUnit(unit)) {
      return false;
    }
  }
  
  return true;
}

/**
 * Validate single unit
 */
function validateUnit(unit: Partial<Unit>): boolean {
  // Check required fields
  if (!unit.name || !unit.type || !unit.role) {
    return false;
  }
  
  // Validate name length
  if (!VALIDATORS.lengthBetween(unit.name, 1, 64)) {
    return false;
  }
  
  // Validate voltage
  if (unit.rated_voltage_v !== undefined) {
    if (!VALIDATORS.isInRange(unit.rated_voltage_v, VALUE_RANGES.VOLTAGE.RATED_MIN, VALUE_RANGES.VOLTAGE.RATED_MAX)) {
      return false;
    }
  }
  
  // Validate current
  if (unit.max_current_a !== undefined) {
    if (!VALIDATORS.isInRange(unit.max_current_a, VALUE_RANGES.CURRENT.MIN, VALUE_RANGES.CURRENT.MAX)) {
      return false;
    }
  }
  
  return true;
}

/**
 * Validate tariffs configuration
 */
function validateTariffsConfig(data: Partial<TariffConfigData>): boolean {
  if (!data.list || !Array.isArray(data.list)) {
    return false;
  }
  
  // Basic validation - ensure each tariff has required fields
  for (const tariff of data.list) {
    if (!tariff.plan_name || !tariff.provider_name || !tariff.plan_type) {
      return false;
    }
  }
  
  return true;
}

/**
 * Validate telemetry configuration
 */
function validateTelemetryConfig(data: Partial<TelemetryConfigData>): boolean {
  // Validate JSON buffer size
  if (data.json_buffer_size !== undefined) {
    if (!VALIDATORS.isInRange(data.json_buffer_size, 1024, 65536)) {
      return false;
    }
  }
  
  // Validate channels
  if (data.channels && Array.isArray(data.channels)) {
    for (const channel of data.channels) {
      if (!['MQTT', 'HTTP', 'WEBSOCKET', 'MODBUS'].includes(channel.protocol)) {
        return false;
      }
    }
  }
  
  return true;
}

/**
 * Validate WiFi configuration
 */
function validateWiFiConfig(data: Partial<WiFiConfigData>): boolean {
  // Validate mode
  if (data.mode && !['AP', 'STA', 'APSTA'].includes(data.mode)) {
    return false;
  }
  
  // Validate AP config
  if (data.ap) {
    if (data.ap.ssid && !VALIDATORS.lengthBetween(data.ap.ssid, 1, 32)) {
      return false;
    }
    if (data.ap.channel !== undefined && !VALIDATORS.isInRange(data.ap.channel, 1, 14)) {
      return false;
    }
  }
  
  // Validate STA config
  if (data.sta) {
    if (data.sta.ssid && !VALIDATORS.lengthBetween(data.sta.ssid, 1, 32)) {
      return false;
    }
  }
  
  return true;
}

/**
 * Calculate power metrics from voltage and current
 */
export function calculatePowerMetrics(
  voltage: number,
  current: number,
  powerFactor: number = 1,
  phaseAngle?: number
): PowerData {
  // Calculate phase angle if not provided
  if (phaseAngle === undefined) {
    phaseAngle = Math.acos(powerFactor) * (180 / Math.PI);
  }
  
  // Calculate power values
  const apparentPower = voltage * current;
  const activePower = apparentPower * powerFactor;
  const reactivePower = apparentPower * Math.sin(phaseAngle * Math.PI / 180);
  
  return {
    activePower: Math.round(activePower * 100) / 100,
    reactivePower: Math.round(reactivePower * 100) / 100,
    apparentPower: Math.round(apparentPower * 100) / 100,
    powerFactor: Math.round(powerFactor * 1000) / 1000,
    phaseAngle: Math.round(phaseAngle * 10) / 10,
  };
}

/**
 * Format timestamp to human-readable string
 */
export function formatTimestamp(
  timestamp: number,
  format: 'date' | 'time' | 'datetime' | 'relative' = 'datetime'
): string {
  // Convert microseconds to milliseconds if needed
  const ms = timestamp > 1e12 ? timestamp / 1000 : timestamp;
  const date = new Date(ms);
  
  switch (format) {
    case 'date':
      return date.toLocaleDateString();
      
    case 'time':
      return date.toLocaleTimeString();
      
    case 'datetime':
      return `${date.toLocaleDateString()} ${date.toLocaleTimeString()}`;
      
    case 'relative':
      return formatRelativeTime(date);
      
    default:
      return date.toISOString();
  }
}

/**
 * Format relative time (e.g., "2 minutes ago")
 */
function formatRelativeTime(date: Date): string {
  const now = new Date();
  const diffMs = now.getTime() - date.getTime();
  const diffSeconds = Math.floor(diffMs / 1000);
  
  if (diffSeconds < 60) {
    return `${diffSeconds} seconds ago`;
  }
  
  const diffMinutes = Math.floor(diffSeconds / 60);
  if (diffMinutes < 60) {
    return `${diffMinutes} minute${diffMinutes > 1 ? 's' : ''} ago`;
  }
  
  const diffHours = Math.floor(diffMinutes / 60);
  if (diffHours < 24) {
    return `${diffHours} hour${diffHours > 1 ? 's' : ''} ago`;
  }
  
  const diffDays = Math.floor(diffHours / 24);
  if (diffDays < 30) {
    return `${diffDays} day${diffDays > 1 ? 's' : ''} ago`;
  }
  
  return date.toLocaleDateString();
}

/**
 * Parse device error to user-friendly message
 */
export function parseDeviceError(error: APIError | Error | unknown): string {
  // Handle APIError
  if (error && typeof error === 'object' && 'code' in error) {
    const apiError = error as APIError;
    return getErrorMessage(apiError.code, apiError.message);
  }
  
  // Handle standard Error
  if (error instanceof Error) {
    return error.message;
  }
  
  // Handle unknown errors
  return 'An unexpected error occurred';
}

/**
 * Get user-friendly error message by code
 */
function getErrorMessage(code: string, defaultMessage: string): string {
  const errorMessages: Record<string, string> = {
    [ERROR_CODES.NETWORK_ERROR]: 'Unable to connect to device. Please check your network connection.',
    [ERROR_CODES.TIMEOUT]: 'Request timed out. The device may be busy or offline.',
    [ERROR_CODES.CONNECTION_LOST]: 'Connection to device lost. Attempting to reconnect...',
    [ERROR_CODES.INVALID_RESPONSE]: 'Received invalid data from device. Please try again.',
    [ERROR_CODES.AUTHENTICATION_FAILED]: 'Authentication failed. Please check your credentials.',
    [ERROR_CODES.PERMISSION_DENIED]: 'You do not have permission to perform this action.',
    [ERROR_CODES.RESOURCE_NOT_FOUND]: 'The requested resource was not found.',
    [ERROR_CODES.DEVICE_OFFLINE]: 'Device is offline. Please check power and network connection.',
    [ERROR_CODES.DEVICE_ERROR]: 'Device reported an error. Please check device logs.',
    [ERROR_CODES.CALIBRATION_FAILED]: 'Calibration failed. Please ensure stable conditions and try again.',
    [ERROR_CODES.INVALID_DATA]: 'Invalid data provided. Please check your input.',
    [ERROR_CODES.DATA_CORRUPTION]: 'Data corruption detected. Please refresh and try again.',
    [ERROR_CODES.STORAGE_FULL]: 'Device storage is full. Please clear old data.',
    [ERROR_CODES.UNKNOWN_ERROR]: defaultMessage || 'An unknown error occurred.',
  };
  
  return errorMessages[code] || defaultMessage || errorMessages[ERROR_CODES.UNKNOWN_ERROR];
}

/**
 * Validate field value against schema
 * @param fieldName - Name of the field to validate
 * @param value - Value to validate
 * @param schema - Validation schema containing field rules
 * @returns Error message if validation fails, null if valid
 */
export function validateField(fieldName: string, value: any, schema: any): string | null {
  const fieldSchema = schema[fieldName];
  if (!fieldSchema) return null;
  
  // Required validation
  if (fieldSchema.required && !VALIDATORS.isRequired(value)) {
    return VALIDATION_MESSAGES.REQUIRED;
  }
  
  // Skip other validations if value is empty and not required
  if (!value && !fieldSchema.required) return null;
  
  // Type validation
  if (fieldSchema.type === 'number' && !VALIDATORS.isNumeric(value)) {
    return VALIDATION_MESSAGES.MUST_BE_NUMBER;
  }
  
  // Length validation
  if (fieldSchema.minLength && !VALIDATORS.minLength(value, fieldSchema.minLength)) {
    return VALIDATION_MESSAGES.MIN_LENGTH(fieldSchema.minLength);
  }
  
  if (fieldSchema.maxLength && !VALIDATORS.maxLength(value, fieldSchema.maxLength)) {
    return VALIDATION_MESSAGES.MAX_LENGTH(fieldSchema.maxLength);
  }
  
  // Range validation
  if (fieldSchema.min !== undefined && value < fieldSchema.min) {
    return VALIDATION_MESSAGES.MIN_VALUE(fieldSchema.min);
  }
  
  if (fieldSchema.max !== undefined && value > fieldSchema.max) {
    return VALIDATION_MESSAGES.MAX_VALUE(fieldSchema.max);
  }
  
  // Pattern validation
  if (fieldSchema.pattern && !fieldSchema.pattern.test(value)) {
    return VALIDATION_MESSAGES.INVALID_FORMAT;
  }
  
  // Enum validation
  if (fieldSchema.enum && !fieldSchema.enum.includes(value)) {
    return VALIDATION_MESSAGES.REQUIRED_SELECT;
  }
  
  // Custom validator
  if (fieldSchema.validator && typeof fieldSchema.validator === 'function') {
    const isValid = fieldSchema.validator(value);
    if (!isValid) {
      return fieldSchema.message || VALIDATION_MESSAGES.INVALID_FORMAT;
    }
  }
  
  return null;
}

/**
 * Format measurement value with unit
 */
export function formatMeasurement(
  value: number,
  type: 'voltage' | 'current' | 'power' | 'energy' | 'frequency' | 'powerFactor' | 'temperature',
  includeUnit: boolean = true
): string {
  const formats = DATA_FORMATS.NUMBERS;
  let formatted: string;
  let unit: string;
  
  switch (type) {
    case 'voltage':
      formatted = value.toFixed(formats.VOLTAGE_DECIMALS);
      unit = 'V';
      break;
      
    case 'current':
      formatted = value.toFixed(formats.CURRENT_DECIMALS);
      unit = 'A';
      break;
      
    case 'power':
      if (value >= 1000) {
        formatted = (value / 1000).toFixed(formats.POWER_DECIMALS);
        unit = 'kW';
      } else {
        formatted = value.toFixed(formats.POWER_DECIMALS);
        unit = 'W';
      }
      break;
      
    case 'energy':
      formatted = value.toFixed(formats.ENERGY_DECIMALS);
      unit = 'kWh';
      break;
      
    case 'frequency':
      formatted = value.toFixed(1);
      unit = 'Hz';
      break;
      
    case 'powerFactor':
      formatted = value.toFixed(3);
      unit = '';
      break;
      
    case 'temperature':
      formatted = value.toFixed(1);
      unit = '°C';
      break;
      
    default:
      formatted = value.toString();
      unit = '';
  }
  
  return includeUnit && unit ? `${formatted} ${unit}` : formatted;
}

/**
 * Calculate energy cost
 */
export function calculateEnergyCost(
  energyKwh: number,
  ratePerKwh: number,
  additionalCharges: number = 0
): number {
  const energyCost = energyKwh * ratePerKwh;
  return Math.round((energyCost + additionalCharges) * 100) / 100;
}

/**
 * Validate IP address
 */
export function isValidIpAddress(ip: string): boolean {
  return VALIDATORS.isIpAddress(ip);
}

/**
 * Validate MAC address
 */
export function isValidMacAddress(mac: string): boolean {
  return VALIDATORS.isMacAddress(mac);
}

/**
 * Generate unique ID
 */
export function generateUniqueId(prefix: string = 'id'): string {
  const timestamp = Date.now().toString(36);
  const randomPart = Math.random().toString(36).substr(2, 9);
  return `${prefix}_${timestamp}_${randomPart}`;
}

/**
 * Deep clone object
 */
export function deepClone<T>(obj: T): T {
  return JSON.parse(JSON.stringify(obj));
}

/**
 * Debounce function
 */
export function debounce<T extends (...args: any[]) => any>(
  func: T,
  delay: number
): (...args: Parameters<T>) => void {
  let timeoutId: NodeJS.Timeout | null = null;
  
  return (...args: Parameters<T>) => {
    if (timeoutId) {
      clearTimeout(timeoutId);
    }
    
    timeoutId = setTimeout(() => {
      func(...args);
    }, delay);
  };
}

/**
 * Throttle function
 */
export function throttle<T extends (...args: any[]) => any>(
  func: T,
  limit: number
): (...args: Parameters<T>) => void {
  let inThrottle: boolean = false;
  
  return (...args: Parameters<T>) => {
    if (!inThrottle) {
      func(...args);
      inThrottle = true;
      setTimeout(() => {
        inThrottle = false;
      }, limit);
    }
  };
}

/**
 * Format file size
 */
export function formatFileSize(bytes: number): string {
  if (bytes === 0) return '0 Bytes';
  
  const k = 1024;
  const sizes = ['Bytes', 'KB', 'MB', 'GB'];
  const i = Math.floor(Math.log(bytes) / Math.log(k));
  
  return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
}

/**
 * Check if value is empty
 */
export function isEmpty(value: any): boolean {
  if (value === null || value === undefined) return true;
  if (typeof value === 'string') return value.trim().length === 0;
  if (Array.isArray(value)) return value.length === 0;
  if (typeof value === 'object') return Object.keys(value).length === 0;
  return false;
}

/**
 * Format currency value
 */
export function formatCurrency(value: number, currency: string = 'EUR'): string {
  return new Intl.NumberFormat('en-US', {
    style: 'currency',
    currency: currency,
  }).format(value);
}

/**
 * Format number with specified decimal places
 */
export function formatNumber(value: number, decimals: number = 2): string {
  return value.toFixed(decimals);
}

/**
 * Format date
 */
export function formatDate(date: Date | number): string {
  const d = typeof date === 'number' ? new Date(date) : date;
  return d.toLocaleDateString();
}

/**
 * Format time
 */
export function formatTime(date: Date | number): string {
  const d = typeof date === 'number' ? new Date(date) : date;
  return d.toLocaleTimeString();
}

/**
 * Format date and time
 */
export function formatDateTime(date: Date | number): string {
  const d = typeof date === 'number' ? new Date(date) : date;
  return `${d.toLocaleDateString()} ${d.toLocaleTimeString()}`;
}

/**
 * Format duration in milliseconds to human readable string
 */
export function formatDuration(ms: number): string {
  const seconds = Math.floor(ms / 1000);
  const minutes = Math.floor(seconds / 60);
  const hours = Math.floor(minutes / 60);
  const days = Math.floor(hours / 24);

  if (days > 0) return `${days}d ${hours % 24}h`;
  if (hours > 0) return `${hours}h ${minutes % 60}m`;
  if (minutes > 0) return `${minutes}m ${seconds % 60}s`;
  return `${seconds}s`;
}

/**
 * Format bytes to human readable string
 */
export function formatBytes(bytes: number): string {
  return formatFileSize(bytes);
}

/**
 * Format percentage
 */
export function formatPercentage(value: number, decimals: number = 1): string {
  return `${(value * 100).toFixed(decimals)}%`;
}

/**
 * Validate email address
 */
export function isValidEmail(email: string): boolean {
  const emailRegex = /^[^\s@]+@[^\s@]+\.[^\s@]+$/;
  return emailRegex.test(email);
}

/**
 * Validate URL
 */
export function isValidUrl(url: string): boolean {
  try {
    new URL(url);
    return true;
  } catch {
    return false;
  }
}

/**
 * Delay function for async operations
 */
export function delay(ms: number): Promise<void> {
  return new Promise(resolve => setTimeout(resolve, ms));
}

/**
 * Retry function with exponential backoff
 */
export async function retry<T>(
  fn: () => Promise<T>,
  maxAttempts: number = 3,
  delayMs: number = 1000
): Promise<T> {
  let lastError: Error;
  
  for (let attempt = 1; attempt <= maxAttempts; attempt++) {
    try {
      return await fn();
    } catch (error) {
      lastError = error instanceof Error ? error : new Error(String(error));
      
      if (attempt === maxAttempts) {
        break;
      }
      
      await delay(delayMs * Math.pow(2, attempt - 1));
    }
  }
  
  throw lastError!;
}