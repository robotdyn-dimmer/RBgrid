/**
 * WebSocket client for RBGrid WebApp
 * Handles real-time communication with ESP32 device
 *
 * Firmware facts this client is built around (rbwebsocket_telemetry.c):
 *  - the device PUSHES a compact telemetry frame every ~3 s (no {type,payload} envelope);
 *  - text commands it understands: get_telemetry / start_telemetry / stop_telemetry; any other
 *    text (incl. an app-level "ping") is only logged — there is NO app-level pong;
 *  - protocol PING frames are answered, but browsers can't send them.
 * So liveness = an inbound-silence watchdog (no frame for TIMEOUTS.SILENCE → reconnect), not ping/pong.
 *
 * Reconnect policy: exponential backoff INITIAL_DELAY (1 s) × MULTIPLIER (2) capped at MAX_DELAY
 * (30 s), ±JITTER, MAX_ATTEMPTS 0 = unlimited (the cap bounds the load on a rebooting hub).
 */

import { tokenStore } from '../core/auth/authWiring';
import {
  TelemetryMessage,
  TelemetryData,
  EventData,
  StatusData,
  ErrorData,
  TelemetrySubscription,
} from '../types/telemetry';
import { WEBSOCKET_SETTINGS, API_ENDPOINTS, ENVIRONMENT } from '../constants';
import { getBaseUrl, onConnectionTeardown } from './connection';

/**
 * WebSocket client configuration
 */
export interface WebSocketConfig {
  url: string;
  port?: number;
  /** Use wss:// instead of ws:// (derived from page protocol for same-origin). */
  secure?: boolean;
  reconnect?: boolean;
  reconnectOptions?: {
    initialDelay: number;
    maxDelay: number;
    multiplier: number;
    /** 0 = unlimited. */
    maxAttempts: number;
    /** ± fraction of the delay (0..1). */
    jitter: number;
  };
  /** Inbound-silence watchdog (replaces ping/pong — see file header). */
  liveness?: {
    /** No message for this long (ms) → the socket is considered dead and is recycled. 0 = off. */
    silenceTimeout: number;
    /** Watchdog tick (ms). */
    checkInterval: number;
    /** A socket stuck in CONNECTING longer than this (ms) is dropped and retried. */
    connectTimeout: number;
  };
  bufferOptions?: {
    maxSize: number;
  };
}

/** Defaults from constants/system.ts — mapped key by key (B-1: the old spread used the wrong keys). */
const DEFAULT_RECONNECT: Required<WebSocketConfig>['reconnectOptions'] = {
  initialDelay: WEBSOCKET_SETTINGS.RECONNECT.INITIAL_DELAY,
  maxDelay: WEBSOCKET_SETTINGS.RECONNECT.MAX_DELAY,
  multiplier: WEBSOCKET_SETTINGS.RECONNECT.MULTIPLIER,
  maxAttempts: WEBSOCKET_SETTINGS.RECONNECT.MAX_ATTEMPTS,
  jitter: WEBSOCKET_SETTINGS.RECONNECT.JITTER,
};
const DEFAULT_LIVENESS: Required<WebSocketConfig>['liveness'] = {
  silenceTimeout: WEBSOCKET_SETTINGS.TIMEOUTS.SILENCE,
  checkInterval: WEBSOCKET_SETTINGS.TIMEOUTS.SILENCE_CHECK,
  connectTimeout: WEBSOCKET_SETTINGS.TIMEOUTS.CONNECTION,
};
const DEFAULT_BUFFER: Required<WebSocketConfig>['bufferOptions'] = {
  maxSize: WEBSOCKET_SETTINGS.BUFFER.MAX_SIZE,
};

/** Use `value` when it's a finite number ≥ min, else the fallback (guards undefined/NaN configs). */
function num(value: unknown, fallback: number, min = 0): number {
  return typeof value === 'number' && Number.isFinite(value) && value >= min ? value : fallback;
}

/**
 * WebSocket connection state
 */
/** A socket that closes this soon after opening without a single frame was refused (auth). */
const AUTH_REJECT_MS = 1500;

export enum WebSocketState {
  CONNECTING = 'CONNECTING',
  CONNECTED = 'CONNECTED',
  DISCONNECTED = 'DISCONNECTED',
  RECONNECTING = 'RECONNECTING',
  ERROR = 'ERROR',
}

/**
 * WebSocket event types
 */
export enum WebSocketEvent {
  CONNECTED = 'connected',
  DISCONNECTED = 'disconnected',
  MESSAGE = 'message',
  ERROR = 'error',
  RECONNECTING = 'reconnecting',
  TELEMETRY = 'telemetry',
  EVENT = 'event',
  STATUS = 'status',
  /** Opened then closed at once with no frame — the hub's read-token refusal (see openSocket). */
  AUTH_REJECTED = 'auth_rejected',
}

/**
 * Message buffer item
 */
interface BufferedMessage {
  type: string;
  data: any;
  timestamp: number;
}

/**
 * Event listener type
 */
type EventListener = (data: any) => void;

type Timer = ReturnType<typeof setTimeout>;

/**
 * RBGrid WebSocket Client
 */
export class RBGridWebSocketClient {
  private ws: WebSocket | null = null;
  private config: Required<WebSocketConfig>;
  private state: WebSocketState = WebSocketState.DISCONNECTED;
  private reconnectAttempts = 0;
  private reconnectTimeout: Timer | null = null;
  private livenessInterval: Timer | null = null;
  private connectTimeout: Timer | null = null;
  private listeners: Map<string, Set<EventListener>> = new Map();
  private messageBuffer: BufferedMessage[] = [];
  private subscriptions: Map<string, TelemetrySubscription> = new Map();
  /** Wall-clock ms of the last inbound message (or of the open). */
  private lastMessageTime = 0;
  /** Auto-reconnect wanted right now (false after an explicit disconnect()/dispose()). */
  private shouldReconnect: boolean;
  private disposed = false;
  private endpoint: string = API_ENDPOINTS.WEBSOCKET.TELEMETRY;
  /** The in-flight connect() promise — returned to concurrent callers (no double sockets). */
  private pendingConnect: Promise<void> | null = null;
  private pendingResolve: (() => void) | null = null;
  private pendingReject: ((e: Error) => void) | null = null;

  constructor(config: WebSocketConfig) {
    const r = config.reconnectOptions;
    const l = config.liveness;
    this.config = {
      url: config.url,
      port: config.port || ENVIRONMENT.WS_PORT,
      secure: config.secure ?? false,
      reconnect: config.reconnect !== false,
      reconnectOptions: {
        initialDelay: num(r?.initialDelay, DEFAULT_RECONNECT.initialDelay, 1),
        maxDelay: num(r?.maxDelay, DEFAULT_RECONNECT.maxDelay, 1),
        multiplier: num(r?.multiplier, DEFAULT_RECONNECT.multiplier, 1),
        maxAttempts: num(r?.maxAttempts, DEFAULT_RECONNECT.maxAttempts),
        jitter: Math.min(1, num(r?.jitter, DEFAULT_RECONNECT.jitter)),
      },
      liveness: {
        silenceTimeout: num(l?.silenceTimeout, DEFAULT_LIVENESS.silenceTimeout),
        checkInterval: num(l?.checkInterval, DEFAULT_LIVENESS.checkInterval, 100),
        connectTimeout: num(l?.connectTimeout, DEFAULT_LIVENESS.connectTimeout),
      },
      bufferOptions: {
        maxSize: num(config.bufferOptions?.maxSize, DEFAULT_BUFFER.maxSize),
      },
    };
    this.shouldReconnect = this.config.reconnect;
  }

  /**
   * Connect to WebSocket server. Idempotent: resolves immediately when connected and returns
   * the same pending promise while a connect is in flight (StrictMode double-effects, several
   * hooks) — there is never more than one live socket per client.
   */
  connect(endpoint: string = this.endpoint): Promise<void> {
    if (this.disposed) return Promise.reject(new Error('WebSocket client disposed'));
    this.shouldReconnect = this.config.reconnect;
    if (this.state === WebSocketState.CONNECTED && this.ws) return Promise.resolve();
    if (this.pendingConnect) return this.pendingConnect;

    const pending = this.newPendingConnect();
    // A socket still exists (CONNECTING, or errored and about to close): don't open a second
    // one — its onopen resolves / its close rejects this promise (and drives the reconnect).
    if (this.ws) return pending;

    this.endpoint = endpoint;
    this.clearReconnectTimer();
    this.openSocket();
    return pending;
  }

  private newPendingConnect(): Promise<void> {
    this.pendingConnect = new Promise<void>((resolve, reject) => {
      this.pendingResolve = resolve;
      this.pendingReject = reject;
    });
    // A connect() that nobody awaits must not surface as an unhandled rejection.
    this.pendingConnect.catch(() => undefined);
    return this.pendingConnect;
  }

  /** Create the socket for the current endpoint and wire its handlers. */
  private openSocket(): void {
    this.state = WebSocketState.CONNECTING;
    const scheme = this.config.secure ? 'wss' : 'ws';
    // Read-token gate (root e139ab5e): the hub can't answer 401 before the upgrade, so the token
    // rides in the query (`?token=`). The app's one token store is used — never logged.
    const token = tokenStore.get();
    const query = token ? `${this.endpoint.includes('?') ? '&' : '?'}token=${encodeURIComponent(token)}` : '';
    const wsUrl = `${scheme}://${this.config.url}:${this.config.port}${this.endpoint}${query}`;

    let ws: WebSocket;
    try {
      ws = new WebSocket(wsUrl);
    } catch (error) {
      this.state = WebSocketState.ERROR;
      this.settleConnect(error instanceof Error ? error : new Error(String(error)));
      this.scheduleReconnect();
      return;
    }
    this.ws = ws;
    this.setupEventHandlers(ws);

    // A socket that never opens (half-open TCP, firewalled host) is recycled.
    const ct = this.config.liveness.connectTimeout;
    if (ct > 0) {
      this.connectTimeout = setTimeout(() => {
        if (this.ws === ws && ws.readyState !== WebSocket.OPEN) {
          this.dropSocket(ws);
          this.handleSocketLoss(ws, { code: 4000, reason: 'connect timeout', wasClean: false });
        }
      }, ct);
    }
  }

  /** Resolve/reject the pending connect() promise (once). */
  private settleConnect(error?: Error): void {
    const resolve = this.pendingResolve;
    const reject = this.pendingReject;
    this.pendingConnect = null;
    this.pendingResolve = null;
    this.pendingReject = null;
    if (error) reject?.(error);
    else resolve?.();
  }

  /**
   * Setup WebSocket event handlers. Every handler ignores events from a socket that is no
   * longer the current one (a replaced/closed socket must not schedule reconnects or emit).
   */
  private setupEventHandlers(ws: WebSocket): void {
    let openedAt = 0;
    let gotFrame = false;
    ws.onopen = () => {
      if (this.ws !== ws) return;
      openedAt = Date.now();
      this.clearConnectTimer();
      this.state = WebSocketState.CONNECTED;
      this.reconnectAttempts = 0;
      this.lastMessageTime = Date.now();
      this.startLiveness();
      this.flushMessageBuffer();
      this.resubscribe();

      this.emit(WebSocketEvent.CONNECTED, { timestamp: Date.now() });
      this.settleConnect();
    };

    ws.onclose = (event) => {
      if (this.ws !== ws) return;
      // «101 then closed at once, no frames» = auth refusal by the read-token gate (root
      // contract test, e139ab5e). Reconnect stays on (backoff) — the token may be entered later.
      if (openedAt && !gotFrame && Date.now() - openedAt < AUTH_REJECT_MS) {
        this.emit(WebSocketEvent.AUTH_REJECTED, { hadToken: tokenStore.has() });
      }
      this.handleSocketLoss(ws, { code: event.code, reason: event.reason, wasClean: event.wasClean });
    };

    ws.onerror = (error) => {
      if (this.ws !== ws) return;
      this.emit(WebSocketEvent.ERROR, error);
      // onclose follows and drives the reconnect; just fail a pending connect() now.
      this.settleConnect(new Error('WebSocket connection error'));
    };

    ws.onmessage = (event) => {
      if (this.ws !== ws) return;
      gotFrame = true;
      this.lastMessageTime = Date.now();
      this.handleMessage(event.data);
    };
  }

  /** Common path for close / connect-timeout / silence: emit, then maybe schedule a reconnect. */
  private handleSocketLoss(ws: WebSocket, info: { code: number; reason: string; wasClean: boolean }): void {
    if (this.ws !== ws) return;
    this.ws = null;
    this.clearConnectTimer();
    this.stopLiveness();
    this.state = WebSocketState.DISCONNECTED;
    this.settleConnect(new Error(`WebSocket closed (${info.code})`));

    this.emit(WebSocketEvent.DISCONNECTED, info);

    // Any close we didn't ask for (incl. a server-side clean close on reboot) → reconnect.
    if (this.shouldReconnect) {
      this.scheduleReconnect();
    }
  }

  /** Detach and close a socket without triggering its handlers. */
  private dropSocket(ws: WebSocket): void {
    ws.onopen = null;
    ws.onclose = null;
    ws.onerror = null;
    ws.onmessage = null;
    try {
      if (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING) {
        ws.close(1000, 'Client disconnect');
      }
    } catch {
      /* closing a CONNECTING socket can throw in some engines — it's being discarded anyway */
    }
  }

  /**
   * Handle incoming WebSocket message
   */
  private handleMessage(data: string): void {
    try {
      const message: TelemetryMessage = JSON.parse(data);

      // Emit general message event
      this.emit(WebSocketEvent.MESSAGE, message);

      // Emit specific event based on message type
      switch (message.type) {
        case 'telemetry':
          this.emit(WebSocketEvent.TELEMETRY, message.payload as TelemetryData);
          break;
        case 'event':
          this.emit(WebSocketEvent.EVENT, message.payload as EventData);
          break;
        case 'status':
          this.emit(WebSocketEvent.STATUS, message.payload as StatusData);
          break;
        case 'error':
          this.emit(WebSocketEvent.ERROR, message.payload as ErrorData);
          break;
      }
    } catch (error) {
      console.error('Failed to parse WebSocket message:', error);
    }
  }

  /**
   * Send message through WebSocket
   */
  send(type: string, data: any): boolean {
    const message = {
      type,
      data,
      timestamp: Date.now(),
    };

    if (this.state === WebSocketState.CONNECTED && this.ws?.readyState === WebSocket.OPEN) {
      try {
        this.ws.send(JSON.stringify(message));
        return true;
      } catch (error) {
        console.error('Failed to send WebSocket message:', error);
        this.bufferMessage(type, data);
        return false;
      }
    } else {
      this.bufferMessage(type, data);
      return false;
    }
  }

  /**
   * Buffer message for later sending (bounded — oldest dropped)
   */
  private bufferMessage(type: string, data: any): void {
    const max = this.config.bufferOptions.maxSize;
    if (max <= 0) return;
    while (this.messageBuffer.length >= max) {
      this.messageBuffer.shift(); // Remove oldest message
    }

    this.messageBuffer.push({
      type,
      data,
      timestamp: Date.now(),
    });
  }

  /**
   * Flush message buffer (on open)
   */
  private flushMessageBuffer(): void {
    if (this.state !== WebSocketState.CONNECTED || this.messageBuffer.length === 0) {
      return;
    }

    const messages = [...this.messageBuffer];
    this.messageBuffer = [];
    // send() re-buffers on failure (bounded), so nothing loops here.
    messages.forEach(message => this.send(message.type, message.data));
  }

  /**
   * Subscribe to telemetry updates
   */
  subscribe(subscription: TelemetrySubscription): void {
    this.subscriptions.set(subscription.subscription_id, subscription);

    if (this.state === WebSocketState.CONNECTED) {
      this.send('subscribe', subscription);
    }
  }

  /**
   * Unsubscribe from telemetry updates
   */
  unsubscribe(subscriptionId: string): void {
    this.subscriptions.delete(subscriptionId);

    if (this.state === WebSocketState.CONNECTED) {
      this.send('unsubscribe', { subscription_id: subscriptionId });
    }
  }

  /**
   * Resubscribe to all active subscriptions
   */
  private resubscribe(): void {
    this.subscriptions.forEach(subscription => {
      this.send('subscribe', subscription);
    });
  }

  /**
   * Start the inbound-silence watchdog (no client ping — see file header).
   */
  private startLiveness(): void {
    this.stopLiveness();
    const { silenceTimeout, checkInterval } = this.config.liveness;
    if (silenceTimeout <= 0) return;

    this.livenessInterval = setInterval(() => {
      const ws = this.ws;
      if (this.state !== WebSocketState.CONNECTED || !ws) return;
      if (Date.now() - this.lastMessageTime > silenceTimeout) {
        console.warn(`WebSocket silent for >${silenceTimeout} ms, reconnecting...`);
        this.dropSocket(ws);
        this.handleSocketLoss(ws, { code: 4001, reason: 'silence timeout', wasClean: false });
      }
    }, checkInterval);
  }

  /**
   * Stop the liveness watchdog
   */
  private stopLiveness(): void {
    if (this.livenessInterval) {
      clearInterval(this.livenessInterval);
      this.livenessInterval = null;
    }
  }

  private clearConnectTimer(): void {
    if (this.connectTimeout) {
      clearTimeout(this.connectTimeout);
      this.connectTimeout = null;
    }
  }

  private clearReconnectTimer(): void {
    if (this.reconnectTimeout) {
      clearTimeout(this.reconnectTimeout);
      this.reconnectTimeout = null;
    }
  }

  /**
   * Delay (ms) before reconnect attempt `attempt` (1-based): initial × multiplier^(attempt-1),
   * capped at maxDelay, ± jitter, never above maxDelay.
   */
  computeReconnectDelay(attempt: number): number {
    const o = this.config.reconnectOptions;
    const baseDelay = Math.min(o.initialDelay * Math.pow(o.multiplier, Math.max(0, attempt - 1)), o.maxDelay);
    const jitter = baseDelay * o.jitter * (Math.random() - 0.5) * 2;
    return Math.min(o.maxDelay, Math.max(0, Math.round(baseDelay + jitter)));
  }

  /**
   * Schedule reconnection attempt
   */
  private scheduleReconnect(): void {
    if (this.disposed || !this.shouldReconnect || this.reconnectTimeout) return;
    const max = this.config.reconnectOptions.maxAttempts;
    if (max > 0 && this.reconnectAttempts >= max) {
      this.state = WebSocketState.ERROR;
      return;
    }

    this.state = WebSocketState.RECONNECTING;
    this.reconnectAttempts++;
    const delay = this.computeReconnectDelay(this.reconnectAttempts);

    this.emit(WebSocketEvent.RECONNECTING, {
      attempt: this.reconnectAttempts,
      delay,
    });

    this.reconnectTimeout = setTimeout(() => {
      this.reconnectTimeout = null;
      if (this.disposed || !this.shouldReconnect || this.ws) return;
      this.connect().catch(() => {
        // Reconnect failed — onclose schedules the next attempt
      });
    }, delay);
  }

  /**
   * Manually reconnect now: recycle the socket, reset the backoff. Auto-reconnect stays ON
   * (B-1: this used to go through disconnect(), which disabled it permanently).
   */
  reconnect(): void {
    if (this.disposed) return;
    this.clearReconnectTimer();
    const ws = this.ws;
    if (ws) {
      this.ws = null;
      this.dropSocket(ws);
    }
    this.clearConnectTimer();
    this.stopLiveness();
    this.settleConnect(new Error('WebSocket reconnect requested'));
    this.state = WebSocketState.DISCONNECTED;
    this.reconnectAttempts = 0;
    this.connect().catch(() => {
      // Reconnect failed — auto-reconnect continues
    });
  }

  /**
   * Disconnect from WebSocket server. Stops auto-reconnect until the next connect().
   */
  disconnect(): void {
    this.shouldReconnect = false;
    this.clearReconnectTimer();
    this.clearConnectTimer();
    this.stopLiveness();

    const ws = this.ws;
    this.ws = null;
    if (ws) this.dropSocket(ws);
    this.settleConnect(new Error('WebSocket disconnected'));

    this.reconnectAttempts = 0;
    this.state = WebSocketState.DISCONNECTED;
  }

  /**
   * Add event listener
   */
  on(event: string, listener: EventListener): void {
    if (!this.listeners.has(event)) {
      this.listeners.set(event, new Set());
    }
    this.listeners.get(event)!.add(listener);
  }

  /**
   * Remove event listener
   */
  off(event: string, listener: EventListener): void {
    const listeners = this.listeners.get(event);
    if (listeners) {
      listeners.delete(listener);
    }
  }

  /**
   * Add one-time event listener
   */
  once(event: string, listener: EventListener): void {
    const onceListener = (data: any) => {
      listener(data);
      this.off(event, onceListener);
    };
    this.on(event, onceListener);
  }

  /**
   * Emit event to listeners
   */
  private emit(event: string, data: any): void {
    const listeners = this.listeners.get(event);
    if (listeners) {
      // Copy: a listener may unsubscribe itself (once) while we iterate.
      Array.from(listeners).forEach(listener => {
        try {
          listener(data);
        } catch (error) {
          console.error(`Error in WebSocket event listener for ${event}:`, error);
        }
      });
    }
  }

  /**
   * Get current connection state
   */
  getState(): WebSocketState {
    return this.state;
  }

  /**
   * Check if connected
   */
  isConnected(): boolean {
    return this.state === WebSocketState.CONNECTED;
  }

  /**
   * Get reconnect attempts
   */
  getReconnectAttempts(): number {
    return this.reconnectAttempts;
  }

  /**
   * Get buffered message count
   */
  getBufferedMessageCount(): number {
    return this.messageBuffer.length;
  }

  /**
   * Clear message buffer
   */
  clearMessageBuffer(): void {
    this.messageBuffer = [];
  }

  /** True once dispose() ran — the client can't be reused. */
  isDisposed(): boolean {
    return this.disposed;
  }

  /**
   * Dispose: close the socket, cancel every timer, drop listeners/subscriptions/buffer.
   * A disposed client never reconnects; get a fresh one from getWebSocketClient().
   */
  dispose(): void {
    if (this.disposed) return;
    this.disconnect();
    this.disposed = true;
    this.listeners.clear();
    this.subscriptions.clear();
    this.messageBuffer = [];
  }

  /**
   * Destroy WebSocket client (alias of dispose())
   */
  destroy(): void {
    this.dispose();
  }
}

// ===== Singleton instances for different endpoints =====

/** Cached clients by endpoint. Each remembers the target it was built for (`key`). */
const wsClients: Map<string, { key: string; client: RBGridWebSocketClient }> = new Map();

/**
 * Resolve the WebSocket host/port/scheme.
 * Defaults to SAME-ORIGIN (whoever served this page — i.e. the device, when the webapp
 * is embedded in firmware) unless an explicit API base is configured (e.g. dev pointing
 * at a device IP via REACT_APP_API_BASE_URL).
 */
function resolveWsTarget(explicitBase?: string): { host: string; port: number; secure: boolean } {
  const base = explicitBase || getBaseUrl();
  if (base) {
    // The device serves the WS on the SAME host:port as its HTTP server (derive from the base URL).
    const secure = /^https:\/\//i.test(base);
    const stripped = base.replace(/^https?:\/\//i, '');
    const host = stripped.replace(/[/:].*$/, '');
    const portMatch = stripped.match(/^[^/]*:(\d+)/);
    const port = portMatch ? Number(portMatch[1]) : (secure ? 443 : 80);
    return { host, port, secure };
  }
  if (typeof window !== 'undefined' && window.location) {
    const secure = window.location.protocol === 'https:';
    const port = window.location.port ? Number(window.location.port) : (secure ? 443 : 80);
    return { host: window.location.hostname, port, secure };
  }
  return { host: 'localhost', port: 80, secure: false };
}

/**
 * Get or create the WebSocket client for an endpoint. The target (host/port/scheme) is
 * re-resolved on every call: if the active device changed since the client was created, the
 * old client is disposed and a new one is built for the new device (B-6 — device B must never
 * show device A's telemetry).
 */
export function getWebSocketClient(
  endpoint: string,
  config?: Partial<WebSocketConfig>
): RBGridWebSocketClient {
  const target = resolveWsTarget(config?.url);
  const key = `${target.secure ? 'wss' : 'ws'}://${target.host}:${target.port}${endpoint}`;
  const cached = wsClients.get(endpoint);
  if (cached && cached.key === key && !cached.client.isDisposed()) {
    return cached.client;
  }
  cached?.client.dispose();
  const client = new RBGridWebSocketClient({
    ...config,
    url: target.host,
    port: target.port,
    secure: target.secure,
  });
  wsClients.set(endpoint, { key, client });
  return client;
}

/**
 * Get telemetry WebSocket client
 */
export function getTelemetryClient(config?: Partial<WebSocketConfig>): RBGridWebSocketClient {
  return getWebSocketClient(API_ENDPOINTS.WEBSOCKET.TELEMETRY, config);
}

/**
 * Get events WebSocket client
 */
export function getEventsClient(config?: Partial<WebSocketConfig>): RBGridWebSocketClient {
  return getWebSocketClient(API_ENDPOINTS.WEBSOCKET.EVENTS, config);
}

/**
 * Get status WebSocket client
 */
export function getStatusClient(config?: Partial<WebSocketConfig>): RBGridWebSocketClient {
  return getWebSocketClient(API_ENDPOINTS.WEBSOCKET.STATUS, config);
}

/**
 * Disconnect AND dispose all WebSocket clients and clear the cache (device switch / logout).
 */
export function disconnectAllClients(): void {
  wsClients.forEach(({ client }) => client.dispose());
  wsClients.clear();
}

// Any connection change (device switch, demo/broker, disconnect) drops every WS client (B-6).
onConnectionTeardown(disconnectAllClients);

/**
 * Default export: a LAZY handle to the telemetry client — resolves getTelemetryClient() on each
 * access, so it always targets the active device (the old eager instance was created at import
 * time against whatever base URL existed then, and survived device switches).
 */
const lazyTelemetryClient: RBGridWebSocketClient = new Proxy({} as RBGridWebSocketClient, {
  get(_target, prop) {
    const client = getTelemetryClient();
    const value = (client as any)[prop];
    return typeof value === 'function' ? value.bind(client) : value;
  },
});

export default lazyTelemetryClient;
