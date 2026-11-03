/**
 * Runtime device connection — the app's first step is choosing a device (enter its IP/URL)
 * or demo mode. This holds the active base URL + demo flag (persisted), and the api/ws layers
 * read it per request so the device target is runtime-configurable (not a build-time hardcode).
 *
 * When the app is served FROM a device (embedded, non-localhost origin) it auto-connects
 * same-origin and skips the connect screen.
 */

export interface Connection {
  /** API base URL. '' = same-origin (served from the device). */
  baseUrl: string;
  /** Demo mode — synthetic data, no device. */
  demo: boolean;
  /** Transport: 'direct' (HTTP/WS) default, or 'mqtt' (broker, one device). */
  kind?: 'direct' | 'mqtt';
  /** MQTT: broker wss URL. */
  brokerUrl?: string;
  /** MQTT: the one device's MAC (topic namespace). */
  deviceMac?: string;
}

const KEY = 'rbgrid.connection';
const listeners = new Set<() => void>();

/**
 * Build profile: `npm run build:embedded` sets REACT_APP_EMBEDDED=true — the UI is installed
 * into the hub's own filesystem (firmware note embed-and-mobile: hybrid, LittleFS), so it talks to
 * the hub that served it, same-origin (no CORS, no mixed content, no device picker).
 */
export const EMBEDDED = process.env.REACT_APP_EMBEDDED === 'true';

/**
 * Build profile: `npm run build:demo` sets REACT_APP_DEMO_ONLY=true — the public demo for the
 * website (operator 20.10): always the built-in demo sites, no device picker, no «switch device».
 */
export const DEMO_ONLY = process.env.REACT_APP_DEMO_ONLY === 'true';

function load(): Connection | null {
  if (DEMO_ONLY) return { baseUrl: '', demo: true };
  // Hosted builds (cloud/domain, mobile wrappers) talk to the device cross-origin and need an
  // explicit device choice (or a persisted one). Deep-links: ?device=<ip>, ?demo.
  // The embedded build defaults to the serving hub (same origin) unless a deep-link says otherwise.
  try {
    if (typeof window !== 'undefined' && window.location) {
      const dev = window.location.search.match(/[?&]device=([^&]+)/);
      if (dev) return { baseUrl: normalizeBaseUrl(decodeURIComponent(dev[1])), demo: false };
      if (/[?&]demo\b/.test(window.location.search)) return { baseUrl: '', demo: true };
    }
    const s = localStorage.getItem(KEY);
    if (s) return JSON.parse(s) as Connection;
    if (EMBEDDED && typeof window !== 'undefined' && window.location?.origin) {
      return { baseUrl: window.location.origin, demo: false };
    }
  } catch {
    /* ignore */
  }
  return null;
}

let current: Connection | null = load();

/**
 * Teardown hooks run BEFORE listeners whenever the active connection changes (device switch,
 * broker/demo switch, disconnect). The WS layer registers disconnectAllClients() here (B-6) —
 * a registry instead of a direct import, since websocket.ts already imports this module.
 */
const teardowns = new Set<() => void>();

/** Register a teardown run on every connection change (returns an unregister fn). */
export function onConnectionTeardown(fn: () => void): () => void {
  teardowns.add(fn);
  return () => { teardowns.delete(fn); };
}

function teardown() {
  teardowns.forEach((t) => {
    try { t(); } catch { /* a failing teardown must not block the switch */ }
  });
}

function notify() {
  listeners.forEach((l) => l());
}

function persist() {
  try {
    if (current) localStorage.setItem(KEY, JSON.stringify(current));
    else localStorage.removeItem(KEY);
  } catch {
    /* ignore */
  }
}

/** Normalize a user-entered host/URL into a base URL (adds http://, strips trailing /). */
export function normalizeBaseUrl(input: string): string {
  let u = (input || '').trim();
  if (!u) return '';
  if (!/^https?:\/\//i.test(u)) u = `http://${u}`;
  return u.replace(/\/+$/, '');
}

export function getConnection(): Connection | null { return current; }
export function isConnected(): boolean { return current !== null; }
export function isDemo(): boolean { return current?.demo === true; }
/** Active API base URL ('' = same-origin). */
export function getBaseUrl(): string { return current?.baseUrl ?? ''; }
/**
 * Whether requests to the device need no CORS preflight. Hubs before 0d67ae6f don't list
 * X-FW-* in theirs, so from another origin (the domain-hosted app, a dev server) the browser
 * drops an upload carrying them as «Network error» — callers retry once without.
 */
export function deviceSameOrigin(): boolean {
  try { return new URL(getBaseUrl() || '/', window.location.href).origin === window.location.origin; } catch { return false; }
}
/** Active transport kind. */
export function getTransportKind(): 'direct' | 'mqtt' { return current?.kind ?? 'direct'; }
export function getBrokerUrl(): string { return current?.brokerUrl ?? ''; }
export function getDeviceMac(): string { return current?.deviceMac ?? ''; }

export function connectDevice(input: string): void {
  teardown(); // drop the previous device's WS clients first (B-6)
  current = { baseUrl: normalizeBaseUrl(input), demo: false, kind: 'direct' };
  persist();
  notify();
}

/** Connect via an MQTT broker (single device, remote). */
export function connectBroker(brokerUrl: string, deviceMac: string): void {
  teardown(); // drop the previous device's WS clients first (B-6)
  current = { baseUrl: '', demo: false, kind: 'mqtt', brokerUrl: brokerUrl.trim(), deviceMac: deviceMac.trim() };
  persist();
  notify();
}

export function connectDemo(): void {
  teardown(); // drop the previous device's WS clients first (B-6)
  current = { baseUrl: '', demo: true };
  persist();
  notify();
}

const LAST_DEVICE_KEY = 'rbgrid.lastDevice';
/** The last directly-connected device's address — the connect screen offers it back
 *  («Сменить» used to land on an unrelated 192.168.1.100 — UX audit HEU-10). */
export function lastDevice(): string {
  try { return localStorage.getItem(LAST_DEVICE_KEY) ?? ''; } catch { return ''; }
}

export function disconnect(): void {
  if (DEMO_ONLY) return; // the demo build has nothing else to connect to
  teardown(); // drop the previous device's WS clients first (B-6)
  if (current && !current.demo && current.baseUrl) {
    try { localStorage.setItem(LAST_DEVICE_KEY, current.baseUrl.replace(/^https?:\/\//, '')); } catch { /* storage off */ }
  }
  current = null;
  persist();
  notify();
}

/** Subscribe to connection changes (returns an unsubscribe fn). */
export function onConnectionChange(fn: () => void): () => void {
  listeners.add(fn);
  return () => { listeners.delete(fn); };
}
