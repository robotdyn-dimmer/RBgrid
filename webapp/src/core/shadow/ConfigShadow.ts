/**
 * ConfigShadow — the settings architecture, modelled on the AWS IoT Device Shadow.
 *
 * Per config domain we hold:
 *   - `reported`: the device's current config (last GET) — the truth.
 *   - `desired`:  the user's edited config — the intent.
 *   - `delta`:    computed = the fields of desired that differ from reported (what to push).
 *   - `version`:  monotonic; bumped on every reported refresh. Stale updates are discarded.
 *   - `metadata`: per-top-level-key last-edited timestamp (timestamps passed in — no Date here).
 *
 * Reconciliation = POST the delta via the transport, then re-pull reported; when
 * reported catches up to desired the delta empties (converged). Some deltas need a
 * device reboot to converge (`requiresReboot`) — those are surfaced and batched.
 *
 * This module is PURE (no transport, no React, no clock) so it is trivially testable;
 * the store/hook layer injects timestamps and drives the transport.
 */
import { JsonObject, computeDelta, deltaPaths, deepEqual, cloneJson } from '../json';

export type ConfigDomain =
  | 'system' | 'hardware' | 'time' | 'tariff' | 'units'
  | 'mqtt' | 'telemetry' | 'wifi' | 'espnow' | 'security';

/**
 * Domains whose change requires a device reboot to converge. Confirmed by root
 * 2026-06-23: reboot needed for wifi (/wifi/save), hardware-config, mqtt-config,
 * espnow add/pair, https-toggle, and *part* of system-config; NO reboot for
 * telemetry-config, tariff, units.
 *
 * This is a coarse domain-level default. Two refinements are pending firmware:
 *  - espnow is ACTION-specific (add/pair reboot; remove/disable/enable/latch don't) —
 *    handled at the espnow-op layer, not here.
 *  - system-config is PARTIAL (only some fields) — needs the per-domain/per-field
 *    `requires_reboot` flag root is adding in P2 (then we read it from the device
 *    instead of this set). [NEEDS ROOT: requires_reboot flag in POST responses + schema.]
 */
export const REBOOT_DOMAINS: ReadonlySet<ConfigDomain> = new Set<ConfigDomain>([
  'wifi',      // POST /wifi/save reboots
  'hardware',  // grid nominals, read at boot
  'mqtt',      // mqtt-config POST → requires_reboot:true
  'security',  // HTTPS/token enablement
  'espnow',    // add/pair register node units on boot (action-specific; coarse here)
]);

/**
 * On-start fields of the endpoints that mix them with live ones (root 18.10 — exactly the
 * fields that make the POST answer requires_reboot:true). The schema's `reboot_fields` wins once
 * the hub sends it; this is the fallback for hubs before that canon.
 */
export const REBOOT_FIELDS_FALLBACK: Readonly<Record<string, readonly string[]>> = {
  system: ['logging_level', 'enable_logging', 'enable_telemetry', 'max_retry_attempts', 'retry_delay_ms', 'auto_start_measurements'],
  /** POST /api/system/time-sync save_config — the hub checks presence, not change. */
  timeSync: ['enable_time_sync', 'primary_source', 'sntp_server'],
};

/**
 * system-config fields the hub stored but nothing read (root 18.10, gone from the API in 7369c64a:
 * logging level / auto-start are hard-set in main.cpp). Hidden for hubs on older canons.
 */
export const SYSTEM_UNUSED_FIELDS: ReadonlySet<string> = new Set([
  'enable_json_compression', 'enable_encryption', 'enable_telemetry', 'max_retry_attempts', 'retry_delay_ms',
  'logging_level', 'enable_logging', 'auto_start_measurements',
]);

export interface ShadowState {
  reported: JsonObject;
  desired: JsonObject;
  delta: JsonObject;
  version: number;
  /** Top-level desired key → last-edit epoch-ms (caller-supplied). */
  metadata: Record<string, number>;
}

/** One domain's shadow. Immutable-friendly: every mutator returns a new instance. */
export class ConfigShadow {
  readonly domain: ConfigDomain;
  readonly reported: JsonObject;
  readonly desired: JsonObject;
  readonly version: number;
  readonly metadata: Record<string, number>;

  constructor(
    domain: ConfigDomain,
    reported: JsonObject = {},
    desired: JsonObject = reported,
    version = 0,
    metadata: Record<string, number> = {},
  ) {
    this.domain = domain;
    this.reported = reported;
    this.desired = desired;
    this.version = version;
    this.metadata = metadata;
  }

  /** The fields of desired that differ from reported (what we'd push). */
  get delta(): JsonObject {
    return computeDelta(this.reported, this.desired);
  }

  /** True when there is something to reconcile. */
  get hasChanges(): boolean {
    return Object.keys(this.delta).length > 0;
  }

  /** Dotted leaf-paths that differ — for the settings diff view. */
  get changedPaths(): string[] {
    return deltaPaths(this.delta);
  }

  /** Whether converging this delta needs a device reboot. */
  get requiresReboot(): boolean {
    return this.hasChanges && REBOOT_DOMAINS.has(this.domain);
  }

  /**
   * Device reported new state (a GET landed). Bumps version. Per AWS semantics,
   * if the incoming version is older than ours, the update is stale — discard it
   * (returns this unchanged). desired is preserved (user intent survives a refresh),
   * unless the user had no pending edits, in which case desired tracks reported.
   */
  withReported(reported: JsonObject, incomingVersion?: number): ConfigShadow {
    if (incomingVersion !== undefined && incomingVersion < this.version) return this;
    const nextVersion = incomingVersion ?? this.version + 1;
    const desired = this.hasChanges ? this.desired : cloneJson(reported);
    return new ConfigShadow(this.domain, reported, desired, nextVersion, this.metadata);
  }

  /** User edited a top-level key. Records the edit timestamp (caller-supplied). */
  withDesired(key: string, value: JsonObject[string], atEpochMs: number): ConfigShadow {
    const desired = { ...this.desired, [key]: value };
    const metadata = { ...this.metadata, [key]: atEpochMs };
    return new ConfigShadow(this.domain, this.reported, desired, this.version, metadata);
  }

  /** Replace the whole desired document (e.g. form-level edit). */
  withDesiredDoc(desired: JsonObject, atEpochMs: number): ConfigShadow {
    const metadata: Record<string, number> = { ...this.metadata };
    for (const k of Object.keys(desired)) {
      if (!deepEqual(this.reported[k] ?? null, desired[k])) metadata[k] = atEpochMs;
    }
    return new ConfigShadow(this.domain, this.reported, desired, this.version, metadata);
  }

  /** Drop pending edits, snap desired back to reported. */
  reset(): ConfigShadow {
    return new ConfigShadow(this.domain, this.reported, cloneJson(this.reported), this.version, {});
  }

  toState(): ShadowState {
    return {
      reported: this.reported,
      desired: this.desired,
      delta: this.delta,
      version: this.version,
      metadata: this.metadata,
    };
  }
}

/** A whole-device shadow = the per-domain shadows + an aggregate reboot flag. */
export class DeviceShadow {
  readonly domains: ReadonlyMap<ConfigDomain, ConfigShadow>;

  constructor(domains: ReadonlyMap<ConfigDomain, ConfigShadow> = new Map()) {
    this.domains = domains;
  }

  get(domain: ConfigDomain): ConfigShadow {
    return this.domains.get(domain) ?? new ConfigShadow(domain);
  }

  set(domain: ConfigDomain, shadow: ConfigShadow): DeviceShadow {
    const next = new Map(this.domains);
    next.set(domain, shadow);
    return new DeviceShadow(next);
  }

  /** Domains with pending changes (for the "N changes" banner). */
  get dirtyDomains(): ConfigDomain[] {
    return Array.from(this.domains.values()).filter((s) => s.hasChanges).map((s) => s.domain);
  }

  /** Whether applying all pending changes will need a reboot. */
  get requiresReboot(): boolean {
    return Array.from(this.domains.values()).some((s) => s.requiresReboot);
  }

  get hasChanges(): boolean {
    return this.dirtyDomains.length > 0;
  }
}
