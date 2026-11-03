/**
 * useHardware — one shared copy of /api/v1/sensors (sensors + rbAmp modules) for the whole app,
 * refreshed every minute; many tiles read it without each fetching. Used to tell a point's module
 * line (BASIC: consumption only, lower accuracy — entities/moduleLine).
 */
import { useSyncExternalStore } from 'react';
import { getApiClient } from '../services/api';
import { isDemo, getBaseUrl, onConnectionChange } from '../services/connection';
import { demoSensors, demoSensorModules } from '../mock/demo';
import type { SensorListEntry, SensorModule } from '../types/sensors';
import { pointHardware, isCurrentOnly, basicReversed, ctRatingOf } from '../entities/moduleLine';

interface Hw { sensors: SensorListEntry[]; modules: SensorModule[] }
const EMPTY: Hw = { sensors: [], modules: [] };
let hw: Hw = EMPTY;
let key = '';
let at = 0;
let inflight = false;
const subs = new Set<() => void>();

async function refresh(): Promise<void> {
  if (inflight) return;
  inflight = true;
  const k = isDemo() ? 'demo' : getBaseUrl();
  try {
    if (isDemo()) hw = { sensors: demoSensors(), modules: demoSensorModules() };
    else {
      const r = await getApiClient().getSensors();
      hw = { sensors: r.data?.sensors ?? [], modules: Array.isArray(r.data?.modules) ? r.data.modules : [] };
    }
    key = k; at = Date.now();
    subs.forEach((f) => f());
  } catch { /* 503 while the hub boots / offline — keep the last copy, retry on next read */ } finally { inflight = false; }
}

onConnectionChange(() => { hw = EMPTY; key = ''; at = 0; subs.forEach((f) => f()); });

function subscribe(cb: () => void): () => void {
  subs.add(cb);
  return () => { subs.delete(cb); };
}
function snapshot(): Hw {
  const k = isDemo() ? 'demo' : getBaseUrl();
  if (k && (k !== key || Date.now() - at > 60_000)) void refresh();
  return hw;
}

export const useHardware = (): Hw => useSyncExternalStore(subscribe, snapshot, snapshot);

/**
 * A CT model change restarts the module (~0.5–1 s, fw ≤ 1.4.19 — root 20.10): its points drop out
 * for a poll or two. Marked here so the live model holds their last reading instead of showing
 * a fault (useTelemetryEntities).
 */
const restartUntil = new Map<number, number>();
export const MODULE_RESTART_GRACE_MS = 8_000;
export function markModuleRestart(module: number | null | undefined, ms = MODULE_RESTART_GRACE_MS): void {
  if (module == null) return;
  restartUntil.set(module, Date.now() + ms);
}
/** Upper-case unit keys on a module inside its restart window. */
export function restartingUnitKeys(sensors: SensorListEntry[], now = Date.now()): Set<string> {
  const mods = new Set(Array.from(restartUntil).filter(([, until]) => until > now).map(([m]) => m));
  if (mods.size === 0) return new Set();
  return new Set(sensors.filter((s) => s.unit_id && mods.has(s.module ?? -1)).map((s) => String(s.unit_id).toUpperCase()));
}

/** Re-read now (after a change the hub reports in /api/v1/sensors, e.g. the CT model). */
export function refreshHardware(): Promise<void> { at = 0; return refresh(); }

/** Is the point on a BASIC module (consumption only)? */
export function useIsBasicPoint(unitKey: string): boolean {
  const { sensors, modules } = useHardware();
  return pointHardware(unitKey, sensors, modules).basic;
}

/**
 * Points on current-only modules (BASIC I1/I2/I3): current monitoring / load control, NOT energy
 * metering (operator 18.10) — kept out of the energy balance and money (overviewData.lineUnits).
 * Read from the shared copy; empty until /api/v1/sensors has been read.
 */
export function currentOnlyUnitKeys(): Set<string> {
  const mods = new Set(hw.modules.filter(isCurrentOnly).map((m) => m.module));
  return new Set(hw.sensors.filter((s) => s.unit_id && mods.has(s.module ?? -1)).map((s) => String(s.unit_id).toUpperCase()));
}

/** Is the point on a current-only module? */
export function useIsCurrentOnlyPoint(unitKey: string): boolean {
  const { sensors, modules } = useHardware();
  return isCurrentOnly(pointHardware(unitKey, sensors, modules).module);
}

/**
 * BASIC point whose CT reads backwards: the load isn't metered. The hub says so (`reversed`,
 * d03f19ed — and then drops the sign of p); before that canon, inferred from a negative p.
 */
export function useBasicReversed(unitKey: string, p: number | null, volts: number | null, ctModels: Record<number, string>, reported?: unknown): boolean {
  const { sensors, modules } = useHardware();
  if (typeof reported === 'boolean') return reported;
  const hw = pointHardware(unitKey, sensors, modules);
  return hw.basic && basicReversed(p, ctRatingOf(hw.sensor, ctModels), volts);
}
