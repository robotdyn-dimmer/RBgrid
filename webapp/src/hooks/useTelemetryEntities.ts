/**
 * useTelemetryEntities — live RBGrid telemetry as the canonical HA-style entity model.
 *
 * Strategy (per root's gaps#1): render the FIRST frame from the REST snapshot
 * (GET /api/telemetry/latest) so there's no blank screen, then switch to the
 * /ws-telemetry stream (3 s push). Frames are normalized via normalizeTelemetryFrame().
 *
 * The device's WS frames are the raw compact shape (no {type,payload} envelope), so we
 * subscribe to the raw MESSAGE event and detect a frame by the presence of `units`.
 *
 * Liveness (review 02 B-7): `conn` turns 'error' when the socket drops (DISCONNECTED) or no
 * frame has arrived for > max(STALE_AFTER_MS, 3 × observed push cadence) — 2 × STALE_AFTER_MS
 * until the cadence is learned (watchdog on the last applied frame), so the
 * «Нет связи» banner and the schemas' `frame.lost` grey-out actually fire. The next frame
 * clears it (back to 'live' / 'snapshot'). The last data is kept — screens grey it out.
 */

import { foldSplitInput } from '../entities/splitInput';
import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { getApiClient } from '../services/api';
import { getTelemetryClient, WebSocketEvent } from '../services/websocket';
import { API_ENDPOINTS } from '../constants';
import { isDemo, getBaseUrl } from '../services/connection';
import { TelemetryFrame } from '../types/telemetry-units';
import { normalizeTelemetryFrame, NormalizedTelemetry } from '../entities/model';
import { demoTelemetryFrame } from '../mock/demo';
import { useHardware, refreshHardware, restartingUnitKeys } from './useHardware';
import type { UnitEntityGroup } from '../entities/model';
import { noChannelUnitKeys } from '../types/sensors';

/** No frame for this long ⇒ connection considered lost (the device pushes every ~3 s). */
export const STALE_AFTER_MS = 15_000;
/** No FIRST frame (REST snapshot or WS) this long after mount ⇒ the hub is unreachable — the
 *  shell shows «Нет связи» with actions instead of an endless spinner (UX audit HEU-01/HOME-01). */
export const FIRST_FRAME_MS = 10_000;
const WATCHDOG_TICK_MS = 2_000;

export type TelemetryConnState = 'connecting' | 'snapshot' | 'live' | 'error';

export interface UseTelemetryEntitiesResult {
  /** Normalized entity model of the latest frame (null until the first frame). */
  data: NormalizedTelemetry | null;
  /** Connection lifecycle: connecting → snapshot (REST) → live (WS). */
  conn: TelemetryConnState;
  /** Wall-clock ms of the last applied frame. */
  lastUpdate: number | null;
  error: string | null;
  /** The hub refused the live stream (read token missing / wrong — root e139ab5e). */
  authRejected: boolean;
}

function isTelemetryFrame(msg: unknown): msg is TelemetryFrame {
  return (
    !!msg &&
    typeof msg === 'object' &&
    typeof (msg as TelemetryFrame).units === 'object' &&
    (msg as TelemetryFrame).units !== null
  );
}

/**
 * The last frame of THIS device, shared by every page (rerun 16.10 SCADA-R02: without a link a
 * jump «События» → «Обзор» showed «Подключение к устройству…» and lost the last values). A page
 * mounting later starts from it — marked by its real age, the watchdog counts from that frame.
 */
let lastShared: { key: string; data: NormalizedTelemetry; at: number } | null = null;
const deviceKey = (): string => (isDemo() ? 'demo' : getBaseUrl());

/** One REST snapshot for every component that mounts at once (root 20.10: Points sent
 *  /api/telemetry/latest twice into the hub's single-threaded httpd queue). */
let snapshot: { key: string; at: number; p: Promise<unknown> } | null = null;
function latestSnapshot(): Promise<unknown> {
  const key = deviceKey();
  if (snapshot && snapshot.key === key && Date.now() - snapshot.at < 3000) return snapshot.p;
  snapshot = { key, at: Date.now(), p: getApiClient().getLatestTelemetry() };
  return snapshot.p;
}

/** Last (`boot_id`, `hw_rev`) seen in a frame (root 18.10, next image: `hw_rev` grows on a module
 *  attached / swapped / gone, a point's `present` change, a /rbamp.json or units-config save) — a
 *  change re-reads /api/v1/sensors at once instead of within the minute. Absent → the minute refresh. */
let lastHwRev: string | null = null;

export function useTelemetryEntities(): UseTelemetryEntitiesResult {
  const cached = lastShared && lastShared.key === deviceKey() ? lastShared : null;
  const [data, setData] = useState<NormalizedTelemetry | null>(cached?.data ?? null);
  const [conn, setConn] = useState<TelemetryConnState>(cached ? 'snapshot' : 'connecting');
  const [lastUpdate, setLastUpdate] = useState<number | null>(cached?.at ?? null);
  const [error, setError] = useState<string | null>(null);
  const mounted = useRef(true);
  const lastFrameAt = useRef<number | null>(cached?.at ?? null);
  /** Observed push cadence (ms) — a slow telemetry profile must not trip the watchdog. */
  const [authRejected, setAuthRejected] = useState(false);
  const cadence = useRef<number>(0);
  const gaps = useRef<number[]>([]);

  const apply = useCallback((frame: TelemetryFrame, live: boolean) => {
    if (!mounted.current) return;
    const now = Date.now();
    if (lastFrameAt.current != null && live) {
      // Cadence = the SHORTEST of the last few gaps, so one long gap after an outage doesn't
      // inflate it, while a genuinely slow profile (every 30 s) is learned within a few frames.
      gaps.current = [...gaps.current.slice(-4), now - lastFrameAt.current];
      cadence.current = Math.min(...gaps.current);
    }
    lastFrameAt.current = now;
    const raw = frame as { hw_rev?: unknown; boot_id?: unknown };
    const rev = typeof raw.hw_rev === 'number' ? `${String(raw.boot_id ?? '')}|${raw.hw_rev}` : null;
    if (rev != null) {
      if (lastHwRev != null && rev !== lastHwRev) void refreshHardware();
      lastHwRev = rev;
    }
    // The input split per phase (fw 1.1.5) is read as one input — the hub's own sum (splitInput).
    const norm = normalizeTelemetryFrame(foldSplitInput(frame));
    lastShared = { key: deviceKey(), data: norm, at: now };
    setData(norm);
    setLastUpdate(now);
    // Once WS frames arrive, stay 'live' even if a later REST snapshot resolves.
    setConn((prev) => (live ? 'live' : prev === 'live' ? 'live' : 'snapshot'));
    setError(null);
    if (live) setAuthRejected(false);
  }, []);

  // Watchdog: no frame for > STALE_AFTER_MS ⇒ 'error'. Counts from mount until the first frame.
  useEffect(() => {
    const startedAt = Date.now();
    const id = setInterval(() => {
      if (!mounted.current) return;
      const since = lastFrameAt.current ?? startedAt;
      // Until the push cadence is known, allow two windows (hub .139 pushes every ~15 s when
      // idle — a single 15 s window would flash the banner between two on-time frames).
      // Before ANY frame: the REST snapshot answers within seconds on a reachable hub.
      const limit = lastFrameAt.current == null ? FIRST_FRAME_MS
        : cadence.current > 0 ? Math.max(STALE_AFTER_MS, 3 * cadence.current) : 2 * STALE_AFTER_MS;
      if (Date.now() - since > limit) setConn((prev) => (prev === 'error' ? prev : 'error'));
    }, WATCHDOG_TICK_MS);
    return () => clearInterval(id);
  }, []);

  useEffect(() => {
    mounted.current = true;

    // Demo mode (dev, no device): feed synthetic frames every 3s.
    if (isDemo()) {
      apply(demoTelemetryFrame(), true);
      const id = setInterval(() => apply(demoTelemetryFrame(), true), 3000);
      return () => { mounted.current = false; clearInterval(id); };
    }

    // 1) Initial frame from the REST snapshot — avoids a blank dashboard before the WS opens.
    latestSnapshot()
      .then((frame) => { if (isTelemetryFrame(frame)) apply(frame, false); })
      .catch(() => { /* non-fatal: the WS may still deliver the first frame */ });

    // 2) Live WS stream. Detect frames on the raw MESSAGE event (compact, no type wrapper).
    const client = getTelemetryClient();
    const onMessage = (msg: unknown) => { if (isTelemetryFrame(msg)) apply(msg, true); };
    const onError = () => { if (mounted.current) setError('telemetry connection error'); };
    // A socket drop is NOT «no connection» yet: the client reconnects and the REST snapshot may
    // still be fresh — the watchdog decides after the stale window (a drop used to flash the
    // «Нет связи» banner on a healthy hub, UX audit HOME-19).
    const onDisconnected = () => { /* watchdog-driven */ };
    // Read-token refusal (101 + immediate close) → say so instead of a generic «no connection».
    const onAuth = () => { if (mounted.current) { setAuthRejected(true); setConn('error'); } };

    client.on(WebSocketEvent.MESSAGE, onMessage);
    client.on(WebSocketEvent.ERROR, onError);
    client.on(WebSocketEvent.DISCONNECTED, onDisconnected);
    client.on(WebSocketEvent.AUTH_REJECTED, onAuth);
    // Frame v2 per socket (canon 274a569c; the hub's global telemetry-config.frame is for
    // MQTT/HA and is never touched here). Older hubs ignore the query and push v1.
    client.connect(`${API_ENDPOINTS.WEBSOCKET.TELEMETRY}?v=2`).catch(() => {
      if (mounted.current) setError('telemetry connection error');
    });

    return () => {
      mounted.current = false;
      client.off(WebSocketEvent.MESSAGE, onMessage);
      client.off(WebSocketEvent.ERROR, onError);
      client.off(WebSocketEvent.DISCONNECTED, onDisconnected);
      client.off(WebSocketEvent.AUTH_REJECTED, onAuth);
      // The telemetry client is a shared singleton — detach listeners but leave it connected.
    };
  }, [apply]);

  // A point whose channel the fitted module lacks (UI3 in place of UI7, .139 18.10) is marked —
  // absent by design, not a sensor that stopped answering.
  const { sensors, modules } = useHardware();
  // The last good reading per point — held over a module restart after a CT model change.
  const lastGood = useRef(new Map<string, UnitEntityGroup>());
  const marked = useMemo(() => {
    if (!data) return data;
    const keys = noChannelUnitKeys(sensors, modules);
    const restarting = restartingUnitKeys(sensors);
    const units = data.units.map((g) => {
      const k = g.unitKey.toUpperCase();
      if (!g.present && keys.has(k)) return { ...g, noChannel: true };
      if (g.present && !g.stale) { lastGood.current.set(k, g); return g; }
      const held = restarting.has(k) ? lastGood.current.get(k) : undefined;
      return held ?? g;
    });
    return { ...data, units };
  }, [data, sensors, modules]);

  return { data: marked, conn, lastUpdate, error, authRejected };
}
