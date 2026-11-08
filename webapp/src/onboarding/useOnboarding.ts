/**
 * useOnboarding — the resumable first-run wizard state (spec §4.1).
 *
 * Steps mirror the spec table (0 Wi-Fi … 7 Done). Completion is DERIVED from live device
 * state (token set? points created? time synced?) so the wizard can be resumed from any
 * device and auto-point at the first unfinished step. The current index is also persisted
 * locally (per device) as a convenience. Hub-side persistence via
 * `system-config.onboarding_step` is a contract item (not yet live) — layered in later.
 *
 * Navigation is always manual (Next/Back/Skip); the derived completion only drives the
 * checkmarks and the initial step. Each step's actual work lives in its panel component.
 */
import { useCallback, useEffect, useState } from 'react';
import { useCapabilities, useDeviceSession } from '../core';
import { isOssBuild } from '../core/schema/capabilities';
import { espnowAvailable } from '../core/espnow/available';
import { useConnection } from '../hooks/useConnection';
import { useLocalStorage } from '../hooks/useLocalStorage';
import { getApiClient } from '../services/api';

export type StepKey =
  | 'wifi' | 'security' | 'object_time' | 'sensors' | 'nodes' | 'tariff' | 'integrations' | 'done';

export interface OnboardingContext {
  connected: boolean;
  adminTokenSet: boolean;
  pointCount: number;
  timeSynced: boolean;
  hasEspnow: boolean;
  isFull: boolean;
  /** The hub carries the user's own plan, not the factory placeholder (root 86f06270). */
  tariffSet: boolean;
  /** The site has a name (not the factory «Default»). */
  siteNamed: boolean;
  siteName?: string;
  /** The hub's POSIX zone (undefined until read). */
  timezone?: string;
  /** The hub's answers are in (counts and flags are not yet «0» while loading). */
  loaded?: boolean;
}

export interface StepDef {
  key: StepKey;
  title: string;
  optional: boolean;
  visible: (c: OnboardingContext) => boolean;
  complete: (c: OnboardingContext) => boolean;
}

const always = () => true;

export const ONBOARDING_STEPS: StepDef[] = [
  { key: 'wifi',         title: 'Connection',    optional: false, visible: always,                 complete: (c) => c.connected },
  // Optional: a home on its own LAN may go without a token; the step says what it guards (W191-15).
  { key: 'security',     title: 'Security',      optional: true,  visible: always,                 complete: (c) => c.adminTokenSet },
  // ✓ by what the user did, not by factory values (fresh hub 17.10, W191-09: «Site & time ✓» on
  // a hub still named «Default», «Tariff ✓» on the factory test plan).
  { key: 'object_time',  title: 'Site & time',   optional: false, visible: always,                 complete: (c) => c.timeSynced && c.siteNamed },
  { key: 'sensors',      title: 'Sensors',       optional: false, visible: always,                 complete: (c) => c.pointCount > 0 },
  { key: 'nodes',        title: 'Nodes',         optional: true,  visible: (c) => c.hasEspnow,      complete: always },
  { key: 'tariff',       title: 'Tariff',        optional: true,  visible: (c) => c.isFull,         complete: (c) => c.tariffSet },
  { key: 'integrations', title: 'Integrations',  optional: true,  visible: always,                 complete: always },
  { key: 'done',         title: 'Done',          optional: false, visible: always,                 complete: () => false },
];

export interface OnboardingState {
  steps: StepDef[];
  ctx: OnboardingContext;
  index: number;
  current: StepDef;
  isComplete: (s: StepDef) => boolean;
  goto: (i: number) => void;
  next: () => void;
  back: () => void;
  refresh: () => void;
  loaded: boolean;
}

export function useOnboarding(): OnboardingState {
  const { connected } = useConnection();
  const caps = useCapabilities();
  const session = useDeviceSession();
  const [storedIndex, setStoredIndex] = useLocalStorage<number>('rbgrid.onboarding.step', 0);

  const [adminTokenSet, setAdminTokenSet] = useState(false);
  const [pointCount, setPointCount] = useState(0);
  const [timeSynced, setTimeSynced] = useState(false);
  const [tariffSet, setTariffSet] = useState(false);
  const [siteNamed, setSiteNamed] = useState(false);
  const [siteName, setSiteName] = useState<string | undefined>(undefined);
  const [timezone, setTimezone] = useState<string | undefined>(undefined);
  // How far the user has been: an optional step is ✓ once passed, not before.
  const [reached, setReached] = useLocalStorage<number>('rbgrid.onboarding.reached', 0);
  const [loaded, setLoaded] = useState(false);
  const [espnowOk, setEspnowOk] = useState(false);
  const [nonce, setNonce] = useState(0);

  useEffect(() => {
    let on = true;
    const api = getApiClient();
    Promise.allSettled([
      session.transport.getTokenRoles(),
      api.getUnitsConfig().catch(() => null),
      api.getLatestTelemetry().catch(() => null),
      api.getSystemSettings().catch(() => null),
      api.getTimeSettings().catch(() => null),
      caps.flags.espnow ? session.transport.getEspnow().catch(() => null) : Promise.resolve(null),
    ]).then(([roles, unitsCfg, frame, sys, time, espnow]) => {
      if (!on) return;
      if (roles.status === 'fulfilled') setAdminTokenSet(roles.value.adminSet);
      if (unitsCfg.status === 'fulfilled' && unitsCfg.value) {
        const list = unitsCfg.value?.data?.units;
        setPointCount(Array.isArray(list) ? list.length : 0);
      }
      if (frame.status === 'fulfilled' && frame.value) {
        const tq = (frame.value as { tq?: number }).tq;
        setTimeSynced(tq === undefined ? false : tq === 1);
        const tariff = (frame.value as { system?: { tariff?: { placeholder?: boolean } } }).system?.tariff;
        setTariffSet(!!tariff && tariff.placeholder !== true);
      }
      if (sys.status === 'fulfilled' && sys.value) {
        const name = (sys.value as unknown as { home_name?: string }).home_name;
        setSiteNamed(!!name && name !== 'Default');
        setSiteName(name && name !== 'Default' ? name : undefined);
      }
      if (time.status === 'fulfilled' && time.value) {
        const tz = (time.value as { config?: { timezone?: unknown } }).config?.timezone;
        setTimezone(typeof tz === 'string' ? tz : undefined);
      }
      // The Nodes step only where the hub can run nodes (fw 1.1.7 `keys`).
      setEspnowOk(espnow.status === 'fulfilled' && !!espnow.value && espnowAvailable(espnow.value.keys));
      setLoaded(true);
    });
    return () => { on = false; };
  }, [session, nonce, caps.flags.espnow]);

  const ctx: OnboardingContext = {
    connected,
    adminTokenSet,
    pointCount,
    timeSynced,
    hasEspnow: caps.flags.espnow && espnowOk,
    isFull: !isOssBuild(caps),
    tariffSet,
    siteNamed,
    siteName,
    timezone,
    loaded,
  };

  const steps = ONBOARDING_STEPS.filter((s) => s.visible(ctx));
  const isComplete = (s: StepDef) => s.complete(ctx) && (!s.optional || s.key === 'tariff' || steps.indexOf(s) < reached);

  const clampedStored = Math.min(Math.max(storedIndex, 0), steps.length - 1);
  const [index, setIndex] = useState(clampedStored);

  const goto = useCallback((i: number) => {
    const clamped = Math.min(Math.max(i, 0), steps.length - 1);
    setIndex(clamped);
    setStoredIndex(clamped);
    setReached((r) => Math.max(r, clamped));
  }, [steps.length, setStoredIndex, setReached]);

  return {
    steps,
    ctx,
    index: Math.min(index, steps.length - 1),
    current: steps[Math.min(index, steps.length - 1)],
    isComplete,
    goto,
    next: () => goto(index + 1),
    back: () => goto(index - 1),
    refresh: () => setNonce((n) => n + 1),
    loaded,
  };
}
