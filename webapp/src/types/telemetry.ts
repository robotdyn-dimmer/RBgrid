/**
 * Telemetry and real-time data types for RBGrid WebApp
 * These types define the structure of real-time measurements and statistics
 */

/**
 * Telemetry data packet
 */
export interface TelemetryData {
  /** Device ID */
  device_id: string;
  /** Packet sequence number */
  sequence: number;
  /** Timestamp when packet was created */
  timestamp: number;
  /** Array of measurements (opaque — websocket.ts casts TelemetryData but never reads this) */
  measurements?: unknown[];
  /** System status */
  system: {
    /** Free heap memory */
    free_heap: number;
    /** CPU temperature */
    cpu_temp: number;
    /** WiFi RSSI */
    wifi_rssi: number;
    /** Uptime in seconds */
    uptime: number;
  };
}

/**
 * WebSocket telemetry message
 */
export interface TelemetryMessage {
  /** Message type */
  type: 'telemetry' | 'event' | 'status' | 'error' | 'pong';
  /** Message payload */
  payload: TelemetryData | EventData | StatusData | ErrorData;
  /** Message timestamp */
  timestamp: number;
}

/**
 * System event data
 */
export interface EventData {
  /** Event identifier */
  event_id: string;
  /** Event type */
  event_type: EventType;
  /** Event severity */
  severity: EventSeverity;
  /** Event source (unit_id or system component) */
  source: string;
  /** Event message */
  message: string;
  /** Additional event data */
  data?: Record<string, any>;
  /** Event timestamp */
  timestamp: number;
}

/**
 * Status update data
 */
export interface StatusData {
  /** Component that changed status */
  component: string;
  /** Previous status */
  old_status: string;
  /** New status */
  new_status: string;
  /** Status change reason */
  reason?: string;
  /** Additional status data */
  data?: Record<string, any>;
}

/**
 * Error data
 */
export interface ErrorData {
  /** Error code */
  code: string;
  /** Error message */
  message: string;
  /** Error source */
  source: string;
  /** Stack trace if available */
  stack?: string;
  /** Additional error context */
  context?: Record<string, any>;
}

/**
 * Event types enumeration
 */
export type EventType = 
  | 'THRESHOLD_EXCEEDED'
  | 'THRESHOLD_RESTORED'
  | 'DEVICE_ONLINE'
  | 'DEVICE_OFFLINE'
  | 'CONFIG_CHANGED'
  | 'CALIBRATION_START'
  | 'CALIBRATION_COMPLETE'
  | 'ERROR_OCCURRED'
  | 'ERROR_CLEARED'
  | 'MAINTENANCE_REQUIRED'
  | 'FIRMWARE_UPDATE'
  | 'POWER_OUTAGE'
  | 'POWER_RESTORED';

/**
 * Event severity levels
 */
export type EventSeverity = 'INFO' | 'WARNING' | 'ERROR' | 'CRITICAL';

/**
 * Telemetry subscription options
 */
export interface TelemetrySubscription {
  /** Subscription ID */
  subscription_id: string;
  /** Unit IDs to subscribe to */
  unit_ids: number[];
  /** Update interval in milliseconds */
  interval_ms: number;
  /** Include system status */
  include_system: boolean;
  /** Include events */
  include_events: boolean;
  /** Quality threshold (0-100) */
  min_quality?: number;
}
