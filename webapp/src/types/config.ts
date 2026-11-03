/**
 * Realigned config-domain types — confirmed per-domain by root 2026-06-20.
 *
 * IMPORTANT: the GET/POST envelope is NOT uniform across config domains (root flagged this
 * as a firmware wart, to be unified later). Each api.ts config method parses its own shape;
 * do not route these through formatApiResponse blindly. This file supersedes the divergent
 * SystemConfigData etc. in types/api.ts (those are @deprecated).
 */

/**
 * Site profile selects the Overview layout (UI-design spec §3 / gap G1).
 * Root-confirmed 2026-10-12: strict parse (unknown value does NOT reset current),
 * default "home", live-applied (POST mirrors to live, GET reflects immediately —
 * ignore the generic `requires_reboot:true` in the system-config POST reply for this field).
 */
export type SiteProfile = 'home' | 'industrial' | 'ev';

/**
 * EV charging config (UI-design spec §4.3 / gap G7). Flat on the hub (NOT per-station;
 * per-station is phase 2 with OCPP). Root-confirmed 2026-10-12: `0` = unset → UI defaults
 * (500 W / 30 min / network tariff). `price_per_kwh` is the station SELL price, one per hub
 * (distinct from the network tariff and from net-metering export). Live-applied like site_profile.
 */
export interface EvConfig {
  /** P above which a station line counts as "charging" (W). 0 = unset → UI default 500. */
  charge_threshold_w: number;
  /** Minutes a station stays "waiting" after a session before "free". 0 = unset → UI default 30. */
  idle_timeout_min: number;
  /** Station sell price per kWh for revenue. 0 = unset → UI falls back to tariff. */
  price_per_kwh: number;
}

/**
 * system-config — GET and POST body are both `{ system: SystemConfig }` (NOT BaseResponse).
 */
export interface SystemConfig {
  /** Config schema version (read-only) */
  version?: string;
  /** Device id (read-only) */
  device_id?: string;
  home_name: string;
  location: string;
  /** UI language code, e.g. "EN" */
  language: string;
  logging_level?: number;
  enable_logging?: boolean;
  enable_telemetry?: boolean;
  max_retry_attempts?: number;
  retry_delay_ms?: number;
  enable_json_compression?: boolean;
  enable_encryption?: boolean;
  auto_start_measurements?: boolean;
  /**
   * Overview scenario (gap G1 — LIVE since canon 8631daf6, 2026-10-12). Round-trips on
   * GET/POST /api/system/system-config and live-applies (the device's `requires_reboot:true`
   * on this field is ignored). Optional only for pre-8631daf6 firmware. See {@link SiteProfile}.
   */
  site_profile?: SiteProfile;
  /** EV charging config (gap G7 — LIVE since canon 8631daf6). Flat; 0/0/0 = not set. See {@link EvConfig}. */
  ev?: EvConfig;
}

/** Envelope of GET/POST /api/system/system-config. */
export interface SystemConfigEnvelope {
  system: SystemConfig;
}

/**
 * time-config — GET = BaseResponse, data:{config, statistics}. No dedicated POST (GET-only);
 * `POST /api/system/time-sync` triggers an NTP sync. `statistics` are read-only counters.
 */
export interface TimeConfigView {
  config: Record<string, unknown>;
  statistics?: Record<string, unknown>;
}

/** telemetry-config — GET = BaseResponse, data below. POST is a firmware gap (#3, read-only for now). */
export interface TelemetryChannelView {
  protocol: string; // MQTT | HTTP | WEBSOCKET
  enabled: boolean;
  send_interval_ms?: number;
  retry_count?: number;
  timeout_ms?: number;
  config?: Record<string, unknown>;
}
export interface TelemetryConfigView {
  telemetry_enabled: boolean;
  json_buffer_size?: number;
  compact_json?: boolean;
  channels: TelemetryChannelView[];
}
