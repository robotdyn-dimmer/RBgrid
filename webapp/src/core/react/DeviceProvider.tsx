/**
 * DeviceProvider — the React binding for the device-management layer.
 *
 * Wraps a single DeviceSession, re-rendering the subtree whenever the session
 * notifies (capabilities/shadow/jobs change). Hooks expose the pieces a page needs:
 *   useCapabilities() · useConfigShadow(domain) · useJobs() · useDeviceSession().
 *
 * Additive: not yet mounted in the app tree — pages migrate onto it in P2 (settings
 * reconciliation) and beyond. Built + typed now so that work is a wiring step, not a
 * design step.
 */
import React, { createContext, useContext, useEffect, useMemo, useReducer, useRef, useState } from 'react';
import { JsonObject } from '../json';
import { DeviceCapabilities } from '../schema/capabilities';
import { ConfigDomain, ConfigShadow } from '../shadow/ConfigShadow';
import { Job } from '../jobs/JobTracker';
import { DeviceTransport, DeviceTarget } from '../transport/types';
import { createTransport } from '../transport/TransportRouter';
import { getTransportKind, getBaseUrl, getBrokerUrl, getDeviceMac } from '../../services/connection';
import { DeviceSession } from '../session/DeviceSession';
import { installBearerAuth } from '../auth/authWiring';
import { provisioningView, ProvisioningView, EMPTY_PROVISIONING_STATUS } from '../provisioning/ProvisioningFSM';
import { Economics, Summary, SummaryPeriod } from '../economics/types';
import { EspnowStatus } from '../espnow/types';

const DeviceSessionContext = createContext<DeviceSession | null>(null);
/**
 * Bumped on every session notify(). The session object itself never changes identity, so
 * without this a notify() re-rendered only the provider and React bailed out of the subtree
 * (same `children`) — e.g. Настройки sat on spinners after their config GETs landed; screens
 * with a live telemetry state (Обзор) only looked fine because they re-render every frame
 * (review 02 B-21). Every useDeviceSession() consumer subscribes to this.
 */
const SessionVersionContext = createContext(0);

export const DeviceProvider: React.FC<{
  /** Override the transport (tests / MQTT). Defaults to the Direct backend. */
  transport?: DeviceTransport;
  children: React.ReactNode;
}> = ({ transport, children }) => {
  const [version, tick] = useReducer((x: number) => x + 1, 0);
  const sessionRef = useRef<DeviceSession | null>(null);
  if (!sessionRef.current) {
    const target: DeviceTarget = getTransportKind() === 'mqtt'
      ? { kind: 'mqtt', brokerUrl: getBrokerUrl(), deviceMac: getDeviceMac() }
      : { kind: 'direct', baseUrl: getBaseUrl() };
    sessionRef.current = new DeviceSession({
      transport: transport ?? createTransport(target),
      onChange: () => tick(),
    });
  }

  // On mount: install Bearer auth on the Direct transport and discover capabilities.
  // Best-effort — schema-less / unreachable devices degrade to baseline capabilities.
  useEffect(() => {
    installBearerAuth();
    const session = sessionRef.current;
    session?.init().catch(() => undefined);
    session?.loadProvisioning().catch(() => undefined);
  }, []);

  return (
    <DeviceSessionContext.Provider value={sessionRef.current}>
      <SessionVersionContext.Provider value={version}>
        {children}
      </SessionVersionContext.Provider>
    </DeviceSessionContext.Provider>
  );
};

export function useDeviceSession(): DeviceSession {
  useContext(SessionVersionContext); // subscribe: re-render on every session change
  const ctx = useContext(DeviceSessionContext);
  if (!ctx) throw new Error('useDeviceSession must be used within a <DeviceProvider>');
  return ctx;
}

export function useCapabilities(): DeviceCapabilities {
  return useDeviceSession().capabilities;
}

export function useJobs(): readonly Job[] {
  return useDeviceSession().jobs.jobs;
}

export interface ProvisioningBinding {
  view: ProvisioningView;
  /** True once a real status has been fetched (vs the empty default). */
  loaded: boolean;
  reload: () => Promise<void>;
}

/** The onboarding/setup view derived from the device's provisioning status. */
export function useProvisioning(): ProvisioningBinding {
  const session = useDeviceSession();
  const status = session.provisioning;
  return {
    view: provisioningView(status ?? EMPTY_PROVISIONING_STATUS),
    loaded: status !== null,
    reload: () => session.loadProvisioning().then(() => undefined),
  };
}

export interface ConfigShadowBinding {
  shadow: ConfigShadow;
  reported: JsonObject;
  desired: JsonObject;
  delta: JsonObject;
  changedPaths: string[];
  hasChanges: boolean;
  requiresReboot: boolean;
  load: () => Promise<void>;
  edit: (key: string, value: JsonObject[string]) => void;
  editDoc: (doc: JsonObject) => void;
  reset: () => void;
  apply: () => Promise<Job | null>;
}

export interface AsyncRead<T> {
  data: T | null;
  loading: boolean;
  reload: () => void;
}

/** Billing/economics for a unit (cumulative). */
export function useEconomics(unit: string | null): AsyncRead<Economics> {
  const session = useDeviceSession();
  const [data, setData] = useState<Economics | null>(null);
  const [loading, setLoading] = useState(false);
  const [nonce, setNonce] = useState(0);
  useEffect(() => {
    if (!unit) { setData(null); return; }
    let on = true;
    setLoading(true);
    session.transport.getEconomics(unit)
      .then((d) => { if (on) setData(d); })
      .catch(() => { if (on) setData(null); })
      .finally(() => { if (on) setLoading(false); });
    return () => { on = false; };
  }, [session, unit, nonce]);
  return { data, loading, reload: () => setNonce((n) => n + 1) };
}

/** Windowed summary totals for a unit + period. */
export function useSummary(unit: string | null, period: SummaryPeriod): AsyncRead<Summary> {
  const session = useDeviceSession();
  const [data, setData] = useState<Summary | null>(null);
  const [loading, setLoading] = useState(false);
  const [nonce, setNonce] = useState(0);
  useEffect(() => {
    if (!unit) { setData(null); return; }
    let on = true;
    setLoading(true);
    session.transport.getSummary(unit, period)
      .then((d) => { if (on) setData(d); })
      .catch(() => { if (on) setData(null); })
      .finally(() => { if (on) setLoading(false); });
    return () => { on = false; };
  }, [session, unit, period, nonce]);
  return { data, loading, reload: () => setNonce((n) => n + 1) };
}

/** ESP-NOW topology (hub + nodes), with a reload. */
export function useEspnow(): AsyncRead<EspnowStatus> & { reloadNow: () => Promise<void> } {
  const session = useDeviceSession();
  const [data, setData] = useState<EspnowStatus | null>(null);
  const [loading, setLoading] = useState(false);
  const [nonce, setNonce] = useState(0);
  const fetchNow = async () => {
    setLoading(true);
    try { setData(await session.transport.getEspnow()); }
    catch { setData(null); }
    finally { setLoading(false); }
  };
  useEffect(() => {
    let on = true;
    setLoading(true);
    session.transport.getEspnow()
      .then((d) => { if (on) setData(d); })
      .catch(() => { if (on) setData(null); })
      .finally(() => { if (on) setLoading(false); });
    return () => { on = false; };
  }, [session, nonce]);
  return { data, loading, reload: () => setNonce((n) => n + 1), reloadNow: fetchNow };
}

/** Bind a config domain to its shadow + the edit/apply actions. */
export function useConfigShadow(domain: ConfigDomain): ConfigShadowBinding {
  const session = useDeviceSession();
  const shadow = session.current.get(domain);
  return useMemo<ConfigShadowBinding>(
    () => ({
      shadow,
      reported: shadow.reported,
      desired: shadow.desired,
      delta: shadow.delta,
      changedPaths: shadow.changedPaths,
      hasChanges: shadow.hasChanges,
      requiresReboot: session.domainNeedsReboot(domain), // device-driven (schema flag wins)
      load: () => session.loadDomain(domain),
      edit: (key, value) => session.edit(domain, key, value),
      editDoc: (doc) => session.editDoc(domain, doc),
      reset: () => session.resetDomain(domain),
      apply: () => session.applyDomain(domain),
    }),
    [session, domain, shadow],
  );
}
