/**
 * Ranges of the numeric settings the generic config editor shows (UX audit W1-7 / T7: the
 * EV «charge threshold» took −50 W, an emptied field became 0 and was written). The hub's
 * `schema.limits.ranges` wins (canon 2d2653ee — the same table its POST check uses, 400
 * OUT_OF_RANGE); the copy below is for hubs before it. Unknown keys still need a number.
 */
import type { DecimalRules } from '../../components/common/DecimalField';
import type { DeviceLimits, HubRange } from './capabilities';

export interface NumberRules extends DecimalRules {
  /** A mirror the hub ignores on POST — shown, not edited. */
  readOnly?: boolean;
}

/** Hub table as of canon 2d2653ee (for older firmware). */
const FALLBACK: Record<string, HubRange> = {
  logging_level: { min: 0, max: 3, decimals: 0 },
  max_retry_attempts: { min: 0, max: 255, decimals: 0 },
  retry_delay_ms: { min: 0, max: 600_000, decimals: 0 },
  charge_threshold_w: { min: 0, max: 100_000, decimals: 0 },
  idle_timeout_min: { min: 0, max: 1440, decimals: 0 },
  price_per_kwh: { min: 0, max: 1000, decimals: 4 },
  nominal_voltage: { min: 90, max: 480, decimals: 0, zero_unset: true },
  nominal_frequency: { min: 45, max: 65, decimals: 0, zero_unset: true },
  nominal_v: { min: 90, max: 480, decimals: 0, read_only: true }, // system.grid mirror — write via hardware-config
  nominal_hz: { min: 45, max: 65, decimals: 0, read_only: true },
  channels_count: { min: 0, max: 8, decimals: 0 },
  json_buffer_size: { min: 1024, max: 65_536, decimals: 0, read_only: true },
};

/** Durations, powers, counts and sizes are never negative. */
const NON_NEGATIVE = /(_ms|_sec|_s|_min|_w|_kw|_count|_size|_bytes|_attempts|_pct|_kwh)$/;

/** Rules for the field `key` (leaf) of config `section`; a hub entry for another section is skipped. */
export function numberRules(key: string, limits?: DeviceLimits, section?: string): NumberRules {
  const hub = limits?.ranges?.[key];
  const r = hub && (!section || !hub.section || hub.section === section) ? hub : FALLBACK[key];
  if (r) {
    return {
      required: true,
      ...(r.min !== undefined ? { min: r.min } : {}),
      ...(r.max !== undefined ? { max: r.max } : {}),
      ...(r.decimals !== undefined ? { decimals: r.decimals } : {}),
      ...(r.zero_unset ? { allowZero: true } : {}),
      ...(r.read_only ? { readOnly: true } : {}),
    };
  }
  return NON_NEGATIVE.test(key) ? { min: 0, required: true } : { required: true };
}
