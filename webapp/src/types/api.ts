/**
 * Base API types for RBGrid WebApp
 * These types define the structure of all API responses from RBPower devices
 */

/**
 * Base response structure for all API endpoints
 * @template T - The type of data contained in the response
 */
export interface BaseResponse<T = any> {
  /** Configuration type identifier */
  type: string;
  /** Protocol version */
  version: string;
  /** Unique device identifier */
  device_id: string;
  /** Local timestamp from device (microseconds) */
  timestamp_local: number;
  /** NTP synchronized timestamp (microseconds) */
  timestamp_ntp: number;
  /** Response data payload */
  data: T;
}

/**
 * System configuration data structure
 */
export interface SystemConfigData {
  system_settings: {
    /** Enable home network features */
    enable_home_network: boolean;
    /** Home/installation name */
    home_name: string;
    /** Physical location */
    location: string;
    /** UI language code */
    language: string;
  };
}

/**
 * Hardware configuration (hardware-config `data`). The product has no built-in ADC (operator
 * 18.10) — only the grid nominals are used; they apply on reboot.
 */
export interface HardwareConfigData {
  network: {
    /** Hz; 0 = not set (hub default). */
    nominal_frequency: number;
    /** V L-N; 0 = not set (hub default). */
    nominal_voltage: number;
  };
}

/**
 * Time configuration data structure
 */
export interface TimeConfigData {
  /** Time synchronization configuration */
  config: {
    /** Enable automatic time synchronization */
    enable_time_sync: boolean;
    /** Primary time source */
    primary_source: TimeSource;
    /** Fallback time source */
    fallback_source: TimeSource;
    /** SNTP server address */
    sntp_server: string;
    /** Backup SNTP server */
    sntp_server_backup: string;
    /** SNTP timeout in milliseconds */
    sntp_timeout_ms: number;
    /** Timezone identifier */
    timezone: string;
    /** Enable daylight saving time */
    enable_dst: boolean;
    /** Resync interval in seconds */
    resync_interval_sec: number;
    /** Maximum allowed time drift in seconds */
    max_drift_sec: number;
    /** ESP-NOW hub MAC address */
    esp_now_hub_mac: string;
    /** ESP-NOW timeout in milliseconds */
    esp_now_timeout_ms: number;
    /** Enable RTC fallback */
    enable_rtc_fallback: boolean;
    /** Enable CPU time fallback */
    enable_cpu_time_fallback: boolean;
    /** Manual time setting in microseconds */
    manual_time_us: number;
    /** Force sync on device startup */
    force_sync_on_startup: boolean;
    /** Enable time sync logging */
    enable_time_logging: boolean;
    /** Number of sync retry attempts */
    sync_retry_count: number;
  };
  /** Time synchronization statistics */
  statistics: {
    /** Current synchronization status */
    current_status: SyncStatus;
    /** Currently active time source */
    active_source: TimeSource | 'RTC' | 'CPU';
    /** Last successful sync timestamp */
    last_sync_timestamp_us: number;
    /** System start time */
    system_start_time_us: number;
    /** Total successful syncs */
    successful_syncs: number;
    /** Total failed sync attempts */
    failed_syncs: number;
    /** Number of fallback activations */
    fallback_activations: number;
    /** Total time corrections made */
    time_corrections_count: number;
    /** Last time correction value in seconds */
    last_time_correction_sec: number;
    /** Maximum observed time drift */
    max_observed_drift_sec: number;
    /** Average sync duration */
    average_sync_duration_ms: number;
    /** SNTP requests sent */
    sntp_requests_sent: number;
    /** SNTP responses received */
    sntp_responses_received: number;
    /** ESP-NOW time requests */
    esp_now_time_requests: number;
  };
}

/**
 * Telemetry configuration data
 */
export interface TelemetryConfigData {
  /** Global telemetry enable flag */
  telemetry_enabled: boolean;
  /** JSON serialization buffer size */
  json_buffer_size: number;
  /** Use compact JSON format */
  compact_json: boolean;
  /** Telemetry channels configuration */
  channels: TelemetryChannel[];
}

/**
 * WiFi configuration data
 */
export interface WiFiConfigData {
  /** WiFi operation mode */
  mode: WiFiMode;
  /** Access Point configuration */
  ap: {
    /** Network SSID */
    ssid: string;
    /** WiFi channel */
    channel: number;
    /** Maximum connections allowed */
    max_connection: number;
    /** Hide SSID broadcast */
    ssid_hidden: boolean;
    /** AP IP address */
    ip_address: string;
    /** Network mask */
    netmask: string;
    /** Gateway address */
    gateway: string;
  };
  /** Station mode configuration */
  sta: {
    /** Connected network SSID */
    ssid: string;
    /** Connection status */
    connected: boolean;
    /** Signal strength (RSSI) */
    rssi: number;
    /** Current channel */
    channel: number;
    /** Access point BSSID */
    bssid: string;
    /** Assigned IP address */
    ip_address: string;
    /** Network mask */
    netmask: string;
    /** Gateway address */
    gateway: string;
    /** Primary DNS server */
    dns_main: string;
    /** Backup DNS server */
    dns_backup: string;
  };
}

/**
 * Response type definitions
 */
export type SystemConfigResponse = BaseResponse<SystemConfigData>;
export type HardwareConfigResponse = BaseResponse<HardwareConfigData>;
export type TimeConfigResponse = BaseResponse<TimeConfigData>;
export type TelemetryConfigResponse = BaseResponse<TelemetryConfigData>;
export type WiFiConfigResponse = BaseResponse<WiFiConfigData>;

/**
 * API error response structure
 */
export interface APIError {
  /** Error code */
  code: string;
  /** Human-readable error message */
  message: string;
  /** Additional error details */
  details?: Record<string, any>;
  /** Request timestamp */
  timestamp: number;
}

/**
 * API endpoint configuration
 */
export interface APIEndpoints {
  /** Base API URL */
  baseUrl: string;
  /** Configuration endpoints */
  config: {
    system: string;
    hardware: string;
    time: string;
    units: string;
    tariffs: string;
    telemetry: string;
    wifi: string;
  };
  /** WebSocket endpoints */
  websocket: {
    telemetry: string;
    events: string;
    status: string;
    port: number;
  };
  /** Device management endpoints */
  device: {
    status: string;
    reboot: string;
    reset: string;
    calibrate: string;
  };
  /** Statistics endpoints */
  statistics: {
    realtime: string;
    hourly: string;
    daily: string;
    monthly: string;
  };
}

/**
 * Time source enumeration
 */
export type TimeSource = 'SNTP' | 'MANUAL' | 'ESP_NOW';

/**
 * Sync status enumeration
 */
export type SyncStatus = 'UNINITIALIZED' | 'INITIALIZING' | 'SYNCING' | 'SYNCED' | 'TIMEOUT' | 'ERROR' | 'FALLBACK';

/**
 * WiFi mode enumeration
 */
export type WiFiMode = 'AP' | 'STA' | 'APSTA';

/**
 * WiFi authentication / encryption mode.
 * Mirrors the firmware GET /wifi/scan/results contract (esp_wifi WIFI_AUTH_*).
 * 'unknown' is the firmware fallback for an unrecognized auth mode.
 */
export type WiFiAuthMode =
  | 'open'
  | 'wep'
  | 'wpa_psk'
  | 'wpa2_psk'
  | 'wpa_wpa2_psk'
  | 'wpa3_psk'
  | 'wpa2_wpa3_psk'
  | 'wpa2_enterprise'
  | 'unknown';

/**
 * A single network found during an async WiFi scan
 * (element of `data.networks` in GET /wifi/scan/results, status="done").
 */
export interface WiFiScanNetwork {
  /** Network SSID; empty string for hidden networks */
  ssid: string;
  /** Access point BSSID, e.g. "aa:bb:cc:dd:ee:ff" */
  bssid: string;
  /** Signal strength in dBm, e.g. -67 */
  rssi: number;
  /** WiFi channel (1..13) */
  channel: number;
  /** Authentication / encryption mode */
  auth_mode: WiFiAuthMode;
}

/** data payload of GET /wifi/scan (HTTP 202, envelope type="wifi_scan") */
export interface WiFiScanStartData {
  status: 'scanning';
}

/**
 * data payload of GET /wifi/scan/results (HTTP 200 always, envelope
 * type="wifi_scan_results"). Discriminated on `status` — branch on the field,
 * not the HTTP code, per the agreed async-scan contract.
 */
export type WiFiScanResultsData =
  | { status: 'scanning' }
  | { status: 'done'; count: number; networks: WiFiScanNetwork[] }
  | { status: 'error'; count: number; networks: WiFiScanNetwork[] };

export type WiFiScanStartResponse = BaseResponse<WiFiScanStartData>;
export type WiFiScanResultsResponse = BaseResponse<WiFiScanResultsData>;

/**
 * Telemetry channel configuration
 */
export interface TelemetryChannel {
  /** Communication protocol */
  protocol: TelemetryProtocol;
  /** Channel enabled status */
  enabled: boolean;
  /** Send interval in milliseconds */
  send_interval_ms: number;
  /** Retry attempts on failure */
  retry_count: number;
  /** Request timeout in milliseconds */
  timeout_ms: number;
  /** Protocol-specific configuration */
  config: {
    /** MQTT broker URL */
    broker_url?: string;
    /** MQTT topic */
    topic?: string;
    /** MQTT QoS level */
    qos?: number;
    /** HTTP endpoint URL */
    endpoint_url?: string;
    /** Authentication token */
    auth_token?: string;
    /** WebSocket server URL */
    server_url?: string;
    /** Connection port */
    port?: number;
    [key: string]: any;
  };
}

/**
 * Telemetry protocol enumeration
 */
export type TelemetryProtocol = 'MQTT' | 'HTTP' | 'WEBSOCKET' | 'MODBUS';

/**
 * Generic API request options
 */
export interface APIRequestOptions {
  /** Request timeout in milliseconds */
  timeout?: number;
  /** Request headers */
  headers?: Record<string, string>;
  /** Abort signal for request cancellation */
  signal?: AbortSignal;
  /** Retry configuration */
  retry?: {
    /** Number of retry attempts */
    attempts: number;
    /** Delay between retries in milliseconds */
    delay: number;
    /** Exponential backoff multiplier */
    backoff?: number;
  };
}

/**
 * Pagination parameters for list endpoints
 */
export interface PaginationParams {
  /** Page number (1-based) */
  page?: number;
  /** Items per page */
  limit?: number;
  /** Sort field */
  sortBy?: string;
  /** Sort direction */
  sortOrder?: 'asc' | 'desc';
}

/**
 * Filter parameters for data queries
 */
export interface FilterParams {
  /** Start timestamp (ISO 8601 or Unix timestamp) */
  startTime?: string | number;
  /** End timestamp (ISO 8601 or Unix timestamp) */
  endTime?: string | number;
  /** Unit IDs to filter */
  unitIds?: number[];
  /** Data aggregation level */
  aggregation?: 'raw' | 'minute' | 'hour' | 'day' | 'month';
}

/**
 * Utility type for API method definitions
 */
export type APIMethod = 'GET' | 'POST' | 'PUT' | 'DELETE' | 'PATCH';

/**
 * Configuration update request structure
 */
export interface ConfigUpdateRequest<T> {
  /** Configuration data to update */
  data: Partial<T>;
  /** Update mode */
  mode?: 'merge' | 'replace';
  /** Validate before applying */
  validate?: boolean;
}

/**
 * Batch operation request
 */
export interface BatchRequest {
  /** Array of operations to perform */
  operations: Array<{
    /** Operation ID for tracking */
    id: string;
    /** HTTP method */
    method: APIMethod;
    /** Endpoint path */
    path: string;
    /** Request body */
    body?: any;
  }>;
  /** Continue on error */
  continueOnError?: boolean;
}

/**
 * Batch operation response
 */
export interface BatchResponse {
  /** Individual operation results */
  results: Array<{
    /** Operation ID */
    id: string;
    /** Success status */
    success: boolean;
    /** Response data or error */
    data?: any;
    /** Error details if failed */
    error?: APIError;
  }>;
  /** Summary statistics */
  summary: {
    /** Total operations */
    total: number;
    /** Successful operations */
    succeeded: number;
    /** Failed operations */
    failed: number;
  };
}