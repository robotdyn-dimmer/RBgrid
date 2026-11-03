/**
 * Central export file for all RBGrid WebApp types
 * Import types from this file for consistent usage across the application
 */

// API types
export * from './api';

// Device and measurement types
export * from './device';

// Tariff and billing types
export * from './tariff';

// Telemetry and real-time data types
export * from './telemetry';

// Re-export commonly used types for convenience
export type {
  // API
  BaseResponse,
  APIEndpoints,
  APIError,
  APIRequestOptions,
} from './api';

export type {
  // Device
  Unit,
  UnitType,
  UnitRole,
  SensorType,
} from './device';

export type {
  // Tariff
  TariffPlan,
  TariffZone,
} from './tariff';

export type {
  // Telemetry
  TelemetryData,
} from './telemetry';

// Combined RBPower configuration type
export interface RBPowerConfig {
  // Configuration data
  system: import('./api').SystemConfigData | null;
  hardware: import('./api').HardwareConfigData | null;
  time: import('./api').TimeConfigData | null;
  units: import('./device').UnitsConfigData | null;
  tariffs: import('./tariff').TariffConfigData | null;
  telemetry: import('./api').TelemetryConfigData | null;
  wifi: import('./api').WiFiConfigData | null;

  // Metadata
  lastUpdated: Record<string, Date>;
  loading: Record<string, boolean>;
  errors: Record<string, string | null>;

  // Connection status
  connectionStatus: 'connected' | 'disconnected' | 'connecting' | 'error';
  deviceId: string | null;
}

// Action types for configuration updates
export type ConfigAction =
  | { type: 'LOAD_CONFIG_START'; configType: string }
  | { type: 'LOAD_CONFIG_SUCCESS'; configType: string; data: any }
  | { type: 'LOAD_CONFIG_ERROR'; configType: string; error: string }
  | { type: 'UPDATE_CONNECTION_STATUS'; status: RBPowerConfig['connectionStatus'] }
  | { type: 'SET_DEVICE_ID'; deviceId: string }
  | { type: 'RESET_CONFIG' }
  | { type: 'CLEAR_ERRORS' };

// WebSocket message types
export type WebSocketMessageType = 'telemetry' | 'event' | 'status' | 'error' | 'config_update';

// Config type keys
export const CONFIG_TYPES = [
  'system',
  'hardware',
  'time',
  'units',
  'tariffs',
  'telemetry',
  'wifi'
] as const;

export type ConfigType = typeof CONFIG_TYPES[number];
