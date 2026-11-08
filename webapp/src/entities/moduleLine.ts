/**
 * rbAmp module lines (root dd861a54, rbamp-dev docs). BASIC is the mass, simpler line:
 *  - consumption only — energy has no export (es = 0); a BASIC point can't be generation / export;
 *  - lower accuracy: ±1 % from ~1.5 % of the CT rating (50 A → 0.75 A); the noise below that is
 *    cut by the HUB (root 18.10: app, HA and MQTT must see the same) — the UI doesn't clamp;
 *  - no module firmware update through the hub (no `ota`), `fw` = a version byte.
 * The line comes from /api/v1/sensors modules[]; a point finds its module by (bus, module).
 */
import type { SensorListEntry, SensorModule } from '../types/sensors';

export const isBasic = (m: SensorModule | null | undefined): boolean => m?.line === 'basic';

/** Current-only modules (I1/I2/I3, variant 4..6): monitoring / load control, not energy metering. */
export const isCurrentOnly = (m: SensorModule | null | undefined): boolean => !!m && (m.n_v ?? 1) === 0 && (m.n_i ?? 0) > 0;

/** The sensor and module behind a point (telemetry / REST key, HEX4). */
export function pointHardware(unitKey: string, sensors: SensorListEntry[], modules: SensorModule[]): { sensor: SensorListEntry | null; module: SensorModule | null; basic: boolean } {
  const key = unitKey.toUpperCase();
  const sensor = sensors.find((s) => (s.unit_id ?? '').toUpperCase() === key) ?? null;
  const module = sensor ? modules.find((m) => m.module === sensor.module) ?? null : null;
  return { sensor, module, basic: sensor?.line ? sensor.line === 'basic' : isBasic(module) };
}

/** CT rating (A) from the model name the hub lists («SCT-013-050» → 50); null when unknown. */
export function ctRatingA(ctCode: number | undefined, models: Record<number, string>): number | null {
  if (!ctCode) return null;
  // «SCT-013-050, 50 A» (d03f19ed) — the amps after the comma; older «SCT-013-050» — the trailing number.
  const name = models[ctCode] ?? '';
  const m = /(\d+(?:[.,]\d+)?)\s*A\s*$/i.exec(name) ?? /(\d+)\s*$/.exec(name);
  const a = m ? Number(m[1].replace(',', '.')) : NaN;
  return Number.isFinite(a) && a > 0 ? a : null;
}

/** The CT list by rating; a channel's current model stays listed even if the hub dropped it. */
export function ctOptions(models: Record<number, string>, current: number): [number, string][] {
  const list = Object.entries(models).map(([k, v]) => [Number(k), v] as [number, string]);
  if (!list.some(([k]) => k === current)) list.push([current, `#${current}`]);
  return list.sort((a, b) => (ctRatingA(a[0], models) ?? 999) - (ctRatingA(b[0], models) ?? 999));
}

/**
 * Sensor class (62f1c9a2, root 18.10): 0 = the module isn't calibrated (no sensor choice),
 * 1 = SCT- (the user's SCT-013, chosen per channel, written into the module), 2 = CT- (factory
 * sensor, calibrated with its channel), 3 = built-in — both fixed. Absent (older canon) = choosable.
 */
export type SensorClass = 'uncalibrated' | 'sct' | 'ct' | 'unknown';
export function sensorClassOf(m: SensorModule | null | undefined): SensorClass {
  const c = m?.sensor_class;
  if (m?.uncalibrated === true || c === 0) return 'uncalibrated';
  if (c === 1) return 'sct';
  if (c === 2 || c === 3) return 'ct';
  return 'unknown';
}

/** The channel's CT model can be chosen: SCT- (or a hub that doesn't say yet). */
export const ctSettable = (m: SensorModule | null | undefined): boolean => {
  if (!m) return false;
  if (typeof m.ct_settable === 'boolean') return m.ct_settable;
  const c = sensorClassOf(m);
  return c === 'sct' || c === 'unknown';
};

/**
 * Accurate range of a CT (rbamp-dev / operator 18.10): the rating is a LIMIT — above it the
 * current is clipped and energy silently under-counted. SCT- clips from ~96 % on a sine (earlier on
 * distorted current): 1.5 %…90 % of the rating; CT- has ~25 % headroom: ~4.3 %…100 %.
 */
export function ctRange(ratingA: number, cls: SensorClass): { lo: number; hi: number } {
  return cls === 'ct' ? { lo: ratingA * 0.043, hi: ratingA } : { lo: ratingA * 0.015, hi: ratingA * 0.9 };
}

/** The CT model name of a channel: the fixed CT- table first for a CT- module, else the SCT list. */
export function ctName(code: number | undefined, m: SensorModule | null | undefined, sct: Record<number, string>, fixed: Record<number, string>): string {
  if (!code) return '—';
  return (ctSettable(m) ? sct[code] ?? fixed[code] : fixed[code] ?? sct[code]) ?? `#${code}`;
}

/**
 * A BASIC channel counts the forward flow only: a CT fitted backwards reads a NEGATIVE live p and
 * the load isn't metered at all (live .139 18.10: −520 W, energy flat). Above the noise floor
 * (1.5 % of the CT; 50 W when the CT is unknown) that's a mis-fitted sensor, not export.
 */
export function basicReversed(p: number | null, ratingA: number | null, volts: number | null): boolean {
  if (p == null || p >= 0) return false;
  const floorW = ratingA ? ratingA * 0.015 * (volts && volts > 0 ? volts : 230) : 50;
  return -p > floorW;
}

/** A channel's CT rating: the hub's own (`ct_rated_a`, d03f19ed), else read from the model name. */
export const ctRatingOf = (s: SensorListEntry | null | undefined, models: Record<number, string>): number | null =>
  typeof s?.ct_rated_a === 'number' && s.ct_rated_a > 0 ? s.ct_rated_a : ctRatingA(s?.ct_code, models);

/** A channel's accurate range: from the hub's clipping start when it gives it (ct_fs_a; the hub warns
 *  at ≥ 90 % of it); null when the hub gives no clip figure. */
export function channelRange(s: SensorListEntry | null | undefined, ratingA: number, cls: SensorClass): { lo: number; hi: number } | null {
  // The upper bound is the hub's own: `ct_fs_a`, where clipping starts (SCT-013 ≈ +16 % over nominal,
  // the 2000:1 clamps ≈ +25 %; rbAmp sensor registry, root 04.11). No hub figure (STD factory sensors,
  // an unknown model) → no range and no breaker warning. The old app-side «90 % of nominal» is gone:
  // the sensors read correctly up to and past nominal (operator 04.11).
  if (!(typeof s?.ct_fs_a === 'number' && s.ct_fs_a > 0)) return null;
  return { lo: ctRange(ratingA, cls).lo, hi: s.ct_fs_a };
}
