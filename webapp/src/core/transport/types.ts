/**
 * Transport contract — one device, two interchangeable backends (operator decision
 * 2026-06-23: HTTP/HTTPS+WS *or* MQTT, exactly one active). The domain layer issues
 * transport-agnostic ops; Direct/Mqtt implement them. P0 covers schema + config
 * get/set (the shadow's needs); telemetry/aggregate/admin ops join the interface as
 * their backends land (P1/P3/P4).
 */
import { JsonObject } from '../json';
import { ConfigDomain } from '../shadow/ConfigShadow';
import { RawSchema } from '../schema/capabilities';
import { ProvisioningStatus } from '../provisioning/ProvisioningFSM';
import { Economics, Summary, SummaryPeriod } from '../economics/types';
import { EspnowStatus, EspnowAction, EspnowActionParams, EspnowActionResult } from '../espnow/types';

/** What a config write answered; `requiresReboot` = the device's own per-write verdict, when it gives one. */
/** The hub's access point (root 20.10, bf4621e4). The password itself is never shown or set by
 *  the app — only the user types one. */
export interface ApStatus {
  ssid: string;
  /** false → the first-login password is still in use. */
  userSet: boolean;
}

export interface SetConfigResult { requiresReboot?: boolean }

export type TransportKind = 'direct' | 'mqtt';

/** API token role (T10). admin = writes; read = history/economics/summary/export + WS. */
export type TokenRole = 'admin' | 'read';

/** GET /api/system/token → which roles have a token set (T10). */
export interface TokenRoles {
  adminSet: boolean;
  readSet: boolean;
}

/** GET/POST /api/system/security (T10). dev_cors=true → `Allow-Origin: *`; false → allow-list echo. */
export interface SecurityConfig {
  dev_cors: boolean;
  origins: string[];
}

/**
 * GET /api/system/backup — the ON-DEVICE restore point (single slot, 6 config files; token
 * files excluded). There is NO bundle download — this is "create restore point", not "download".
 * `meta` is additive (next firmware canon); its `ts` is wall-clock (uptime-epoch pre-SNTP).
 */
export interface BackupStatus {
  exists: boolean;
  files: number;
  meta?: { ts: number; fw: string; files: number };
}

/** POST /api/system/restore result. Does NOT self-reboot → caller invokes /api/system/reboot. */
export interface RestoreResult {
  status: string;
  filesRestored: number;
  requiresReboot: boolean;
}

/** POST /api/system/ota result. ⚠ The device SELF-reboots ~1s after a 200 → poll GET /version. */
export interface OtaResult {
  status: string;
  message: string;
  partition: string;
}

/** Options for an OTA upload. `onProgress` fires on upload events (there is no in-band progress). */
export interface OtaOptions {
  /** Exactly 64 hex chars; a mismatch aborts with 400 BEFORE activation. */
  sha256?: string;
  /** Checked against the image's esp_app_desc before set_boot. */
  version?: string;
  onProgress?: (fraction: number) => void;
}

export interface DeviceTarget {
  kind: TransportKind;
  /** Direct: device base URL, e.g. "http://192.168.1.110" / "https://…". */
  baseUrl?: string;
  /** MQTT: broker WebSocket URL — "ws://host:9001" (LAN) or "wss://broker.example:8884" (TLS). */
  brokerUrl?: string;
  /** MQTT: the one device's identity (topic namespace `rbgrid/<mac>/…`). */
  deviceMac?: string;
  /** Optional bearer token / broker credential ref (never logged). */
  token?: string;
}

export interface DeviceTransport {
  readonly kind: TransportKind;
  /** Capability descriptor; null when the device predates the schema endpoint. */
  getSchema(): Promise<RawSchema | null>;
  /** Reported config for a domain (inner config object, envelope unwrapped). */
  getConfig(domain: ConfigDomain): Promise<JsonObject>;
  /** Persist a domain's config (the desired doc / delta-applied doc). */
  setConfig(domain: ConfigDomain, doc: JsonObject): Promise<SetConfigResult | void>;
  /** POST mqtt-config {broker_uri} (apply-on-restart). Optional: transports without it → read-only. */
  setMqttBroker?(uri: string): Promise<SetConfigResult>;
  /**
   * Patch ONE unit's declarative fields (POST /api/v1/units/{hex}, canon a3b5fc62): any of
   * name, short_name, phase, rated_power_w, max_current_a, role. Live + persisted, atomic.
   * Optional: transports/firmware without it → DeviceSession falls back to the full units array.
   */
  patchUnit?(hexId: string, patch: JsonObject): Promise<JsonObject>;
  /** Onboarding status (GET /api/system/provisioning); null when unsupported/unreachable. */
  getProvisioning(): Promise<ProvisioningStatus | null>;
  /** Billing/economics for a unit (GET /api/v1/economics, cumulative); null on error. */
  getEconomics(unit: string): Promise<Economics | null>;
  /** Windowed summary for a unit (GET /api/v1/summary); null on error. */
  getSummary(unit: string, period: SummaryPeriod): Promise<Summary | null>;
  /** Switch the live telemetry profile (POST /api/system/telemetry-config {profile}). */
  setTelemetryProfile(profile: 'lean' | 'full'): Promise<void>;
  /** Whether an (admin) API token is set (GET /api/system/token → {token_set}). */
  getTokenStatus(): Promise<boolean>;
  /** Which roles have a token set (GET /api/system/token → {token_set, read_token_set}) — T10. */
  getTokenRoles(): Promise<TokenRoles>;
  /**
   * Set / rotate (Bearer) / clear (empty) an API token (POST /api/system/token {token, role}).
   * `role` defaults to 'admin' (device default). Direct/HTTP only.
   */
  setToken(token: string, role?: TokenRole): Promise<void>;
  /** The hub's own Wi-Fi access point (GET /api/system/ap, admin): SSID and whether its password
   *  was set by the user; null when unsupported / not allowed. */
  getAp(): Promise<ApStatus | null>;
  /** Set the AP password (POST /api/system/ap {password}, 8–63 printable ASCII; "" = back to the
   *  first-login password). Applies at once — AP clients reconnect. Direct/HTTP only. */
  /** `applied: false` = the hub takes the new password after a restart (root 03.11). */
  setApPassword(password: string): Promise<{ applied?: boolean }>;
  /** CORS/security config (GET /api/system/security); null when unsupported. Admin-gated. */
  getSecurity(): Promise<SecurityConfig | null>;
  /** Persist CORS/security config (POST /api/system/security). Admin-gated. Direct/HTTP only. */
  setSecurity(cfg: SecurityConfig): Promise<void>;
  /** On-device restore-point status (GET /api/system/backup); null when unsupported. */
  getBackupStatus(): Promise<BackupStatus | null>;
  /** Snapshot current configs into the restore-point slot (POST /api/system/backup, empty body). */
  createBackup(): Promise<{ filesBackedUp: number }>;
  /** Restore from the slot (POST /api/system/restore, empty body). Caller then reboots. Direct/HTTP only. */
  restoreBackup(): Promise<RestoreResult>;
  /** Upload a firmware image (POST /api/system/ota, raw .bin). Device self-reboots after 200. Direct/HTTP only. */
  postOta(image: ArrayBuffer | Blob, opts?: OtaOptions): Promise<OtaResult>;
  /** ESP-NOW topology (GET /api/v1/espnow); null when unsupported/unreachable. */
  getEspnow(): Promise<EspnowStatus | null>;
  /** ESP-NOW node action (POST /api/v1/espnow {action,...}); auth-guarded. */
  espnowAction(action: EspnowAction, params?: EspnowActionParams): Promise<EspnowActionResult>;
  /** Streamed CSV export (GET /api/v1/export.csv); HTTP/Direct only. null on error. */
  exportCsv(params: { unit: string; period: string; type: string; from?: number; to?: number }): Promise<Blob | null>;
}

export class UnsupportedDomainError extends Error {
  constructor(public readonly domain: ConfigDomain, kind: TransportKind) {
    super(`Config domain "${domain}" is not available over the ${kind} transport (yet).`);
    this.name = 'UnsupportedDomainError';
  }
}

export class NotImplementedError extends Error {
  constructor(op: string, kind: TransportKind) {
    super(`${kind} transport: ${op} not implemented yet.`);
    this.name = 'NotImplementedError';
  }
}
