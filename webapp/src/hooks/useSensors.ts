/**
 * useSensors — load the sensor registry (GET /api/v1/sensors) with the readiness-gate.
 *
 * Per root's gaps#1: ~16 s post-reboot the endpoint returns 503 {status:"initializing"}.
 * We must NOT render "no devices" then — show "initializing" and retry with backoff.
 * Three terminal states: initializing (retry) / ready+empty (truly none) / ready+list.
 */
import { useCallback, useEffect, useRef, useState } from 'react';
import { AxiosError } from 'axios';
import { getApiClient } from '../services/api';
import { isDemo } from '../services/connection';
import { SensorListEntry, SensorModule } from '../types/sensors';
import { demoSensors, demoSensorModules } from '../mock/demo';

export type SensorsState = 'loading' | 'initializing' | 'ready' | 'error';

export interface UseSensorsResult {
  sensors: SensorListEntry[];
  /** rbAmp modules (delta №5); empty on firmware that doesn't report them. */
  modules: SensorModule[];
  state: SensorsState;
  /** v1.1.2: seconds without a module answer (≥ 30 s) — the bus is stuck; null otherwise. */
  busStalledS: number | null;
  /** v1.1.2: seconds a bus line is held low (≥ 15 s) — clears by itself once released; null otherwise. */
  busLineLowS: number | null;
  error: string | null;
  reload: () => void;
}

const RETRY_MS = 2000;

export function useSensors(): UseSensorsResult {
  const [sensors, setSensors] = useState<SensorListEntry[]>([]);
  const [modules, setModules] = useState<SensorModule[]>([]);
  const [state, setState] = useState<SensorsState>('loading');
  const [busStalledS, setBusStalledS] = useState<number | null>(null);
  const [busLineLowS, setBusLineLowS] = useState<number | null>(null);
  const [error, setError] = useState<string | null>(null);
  const mounted = useRef(true);
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);

  const load = useCallback(async () => {
    if (isDemo()) {
      if (mounted.current) { setSensors(demoSensors()); setModules(demoSensorModules()); setState('ready'); setError(null); }
      return;
    }
    try {
      const res = await getApiClient().getSensors();
      if (!mounted.current) return;
      setSensors(res.data?.sensors ?? []);
      setModules(Array.isArray(res.data?.modules) ? res.data.modules : []);
      setBusStalledS(typeof res.data?.bus_stalled_s === 'number' && res.data.bus_stalled_s > 0 ? res.data.bus_stalled_s : null);
      setBusLineLowS(typeof res.data?.bus_line_low_s === 'number' && res.data.bus_line_low_s > 0 ? res.data.bus_line_low_s : null);
      setState('ready');
      setError(null);
    } catch (err) {
      if (!mounted.current) return;
      const ax = err as AxiosError<unknown>;
      if (ax.response?.status === 503) {
        // Device still initializing — retry, keep any prior list.
        setState('initializing');
        timer.current = setTimeout(load, RETRY_MS);
      } else {
        setState('error');
        setError('Failed to load sensors');
      }
    }
  }, []);

  useEffect(() => {
    mounted.current = true;
    load();
    return () => {
      mounted.current = false;
      if (timer.current) clearTimeout(timer.current);
    };
  }, [load]);

  return { sensors, modules, state, busStalledS, busLineLowS, error, reload: load };
}
