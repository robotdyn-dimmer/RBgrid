/**
 * useDisplayMode — the «Мощность ⇄ Деньги» lens (operator 2026-10-13, review 10).
 *
 * A per-user VIEW preference (localStorage, per connected device), NEVER a device setting —
 * the opposite of site_profile. Defaults per scenario: дом / станции → деньги, предприятие →
 * мощность. Money mode is only offered when the device has an active tariff (live
 * `system.tariff.buy_rate > 0`) and is not an OSS build; otherwise it is forced to power.
 * Operator 2026-10-13: the lens switches every Overview card except the flow schemas; limits,
 * phase currents and power quality keep their physical unit inside those cards.
 */
import { useCallback, useEffect, useState } from 'react';
import { SiteProfile } from '../types/config';
import { getBaseUrl, isDemo } from '../services/connection';

export type DisplayMode = 'power' | 'money';

const DEFAULT_BY_SCENARIO: Record<SiteProfile, DisplayMode> = { home: 'money', industrial: 'power', ev: 'money' };

const keyFor = (): string => `rbgrid.displayMode.${isDemo() ? 'demo' : getBaseUrl() || 'local'}`;

function read(): DisplayMode | null {
  try {
    const v = localStorage.getItem(keyFor());
    return v === 'power' || v === 'money' ? v : null;
  } catch {
    return null;
  }
}

export interface DisplayModeState {
  /** Effective mode (forced to 'power' when money is unavailable). */
  mode: DisplayMode;
  /** Whether the money lens can be offered at all. */
  moneyAvailable: boolean;
  setMode: (m: DisplayMode) => void;
}

export function useDisplayMode(scenario: SiteProfile, moneyAvailable: boolean): DisplayModeState {
  const [stored, setStored] = useState<DisplayMode | null>(read);

  // Re-read when the connected device changes (different key).
  useEffect(() => { setStored(read()); }, []);

  const setMode = useCallback((m: DisplayMode) => {
    setStored(m);
    try { localStorage.setItem(keyFor(), m); } catch { /* storage unavailable → session only */ }
  }, []);

  const preferred = stored ?? DEFAULT_BY_SCENARIO[scenario];
  return { mode: moneyAvailable ? preferred : 'power', moneyAvailable, setMode };
}
