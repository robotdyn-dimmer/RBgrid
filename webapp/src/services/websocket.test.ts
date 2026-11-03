import { RBGridWebSocketClient, WebSocketState, WebSocketEvent, getTelemetryClient, disconnectAllClients } from './websocket';
import { connectDevice, disconnect } from './connection';

/** Minimal controllable WebSocket stand-in. */
class FakeWebSocket {
  static CONNECTING = 0;
  static OPEN = 1;
  static CLOSING = 2;
  static CLOSED = 3;
  static instances: FakeWebSocket[] = [];

  readyState = FakeWebSocket.CONNECTING;
  sent: string[] = [];
  closed = false;
  onopen: ((ev?: unknown) => void) | null = null;
  onclose: ((ev: { code: number; reason: string; wasClean: boolean }) => void) | null = null;
  onerror: ((ev?: unknown) => void) | null = null;
  onmessage: ((ev: { data: string }) => void) | null = null;

  constructor(public url: string) {
    FakeWebSocket.instances.push(this);
  }
  send(data: string) { this.sent.push(data); }
  close() { this.closed = true; this.readyState = FakeWebSocket.CLOSED; }

  // test drivers
  open() { this.readyState = FakeWebSocket.OPEN; this.onopen?.(); }
  message(obj: unknown) { this.onmessage?.({ data: JSON.stringify(obj) }); }
  serverClose(wasClean = false) {
    this.readyState = FakeWebSocket.CLOSED;
    this.onclose?.({ code: wasClean ? 1000 : 1006, reason: '', wasClean });
  }
  fail() { this.onerror?.({}); this.serverClose(false); }
}

const realWS = (global as any).WebSocket;

beforeEach(() => {
  FakeWebSocket.instances = [];
  (global as any).WebSocket = FakeWebSocket;
  jest.useFakeTimers();
  jest.spyOn(Math, 'random').mockReturnValue(0.5); // jitter term = 0
});

afterEach(() => {
  disconnectAllClients();
  jest.useRealTimers();
  jest.restoreAllMocks();
  (global as any).WebSocket = realWS;
});

const live = () => FakeWebSocket.instances.filter((w) => !w.closed && w.readyState !== FakeWebSocket.CLOSED);

describe('RBGridWebSocketClient — B-1', () => {
  it('maps settings: backoff 1s ×2 capped at 30s, unlimited attempts', () => {
    const c = new RBGridWebSocketClient({ url: 'h', port: 80 });
    expect([1, 2, 3, 4, 5, 6, 7, 10].map((n) => c.computeReconnectDelay(n)))
      .toEqual([1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000]);
  });

  it('jitter stays within ±30% and never exceeds the cap', () => {
    const c = new RBGridWebSocketClient({ url: 'h', port: 80 });
    (Math.random as jest.Mock).mockReturnValue(0);
    expect(c.computeReconnectDelay(1)).toBe(700);
    (Math.random as jest.Mock).mockReturnValue(0.999999);
    expect(c.computeReconnectDelay(1)).toBe(1300);
    expect(c.computeReconnectDelay(20)).toBe(30000);
  });

  it('does not open a second socket while CONNECTING (StrictMode double connect)', async () => {
    const c = new RBGridWebSocketClient({ url: 'h', port: 80 });
    const p1 = c.connect('/ws-telemetry');
    const p2 = c.connect('/ws-telemetry');
    expect(FakeWebSocket.instances).toHaveLength(1);
    expect(p2).toBe(p1);
    FakeWebSocket.instances[0].open();
    await expect(p1).resolves.toBeUndefined();
    await c.connect();
    expect(FakeWebSocket.instances).toHaveLength(1);
    expect(c.getState()).toBe(WebSocketState.CONNECTED);
  });

  it('never sends an app-level ping (firmware has no pong) and survives a steady 3s push', () => {
    const c = new RBGridWebSocketClient({ url: 'h', port: 80 });
    c.connect('/ws-telemetry');
    const ws = FakeWebSocket.instances[0];
    ws.open();
    for (let i = 0; i < 20; i++) { jest.advanceTimersByTime(3000); ws.message({ units: {} }); }
    expect(ws.sent.filter((s) => s.includes('ping'))).toHaveLength(0);
    expect(FakeWebSocket.instances).toHaveLength(1);
    expect(c.isConnected()).toBe(true);
  });

  it('silence watchdog recycles a socket that stopped delivering frames', () => {
    jest.spyOn(console, 'warn').mockImplementation(() => undefined);
    const c = new RBGridWebSocketClient({ url: 'h', port: 80 });
    c.connect('/ws-telemetry');
    FakeWebSocket.instances[0].open();
    jest.advanceTimersByTime(20000); // > 15 s silence, checked every 5 s
    expect(FakeWebSocket.instances[0].closed).toBe(true);
    expect(c.getState()).toBe(WebSocketState.RECONNECTING);
    jest.advanceTimersByTime(1000);
    expect(FakeWebSocket.instances).toHaveLength(2);
  });

  it('reconnects with exponential backoff — no tight loop on an unreachable device', () => {
    const c = new RBGridWebSocketClient({ url: 'h', port: 80 });
    const delays: number[] = [];
    c.on(WebSocketEvent.RECONNECTING, (e) => delays.push(e.delay));
    c.connect('/ws-telemetry');
    for (let i = 0; i < 7; i++) {
      FakeWebSocket.instances[FakeWebSocket.instances.length - 1].fail();
      const before = FakeWebSocket.instances.length;
      jest.advanceTimersByTime(delays[delays.length - 1] - 1);
      expect(FakeWebSocket.instances).toHaveLength(before); // nothing before the delay elapses
      jest.advanceTimersByTime(1);
      expect(FakeWebSocket.instances).toHaveLength(before + 1);
    }
    expect(delays).toEqual([1000, 2000, 4000, 8000, 16000, 30000, 30000]);
    expect(live()).toHaveLength(1);
  });

  it('reconnects after a server-side CLEAN close too (device reboot)', () => {
    const c = new RBGridWebSocketClient({ url: 'h', port: 80 });
    c.connect('/ws-telemetry');
    FakeWebSocket.instances[0].open();
    FakeWebSocket.instances[0].serverClose(true);
    jest.advanceTimersByTime(1000);
    expect(FakeWebSocket.instances).toHaveLength(2);
  });

  it('reconnect() keeps auto-reconnect enabled', () => {
    const c = new RBGridWebSocketClient({ url: 'h', port: 80 });
    c.connect('/ws-telemetry');
    FakeWebSocket.instances[0].open();
    c.reconnect();
    expect(FakeWebSocket.instances[0].closed).toBe(true);
    expect(FakeWebSocket.instances).toHaveLength(2);
    FakeWebSocket.instances[1].open();
    FakeWebSocket.instances[1].serverClose(false); // drop after a manual reconnect
    jest.advanceTimersByTime(1000);
    expect(FakeWebSocket.instances).toHaveLength(3);
  });

  it('disconnect() stops reconnecting; a later connect() re-enables it', () => {
    const c = new RBGridWebSocketClient({ url: 'h', port: 80 });
    c.connect('/ws-telemetry');
    FakeWebSocket.instances[0].open();
    c.disconnect();
    jest.advanceTimersByTime(60000);
    expect(FakeWebSocket.instances).toHaveLength(1);
    c.connect();
    FakeWebSocket.instances[1].open();
    FakeWebSocket.instances[1].serverClose(false);
    jest.advanceTimersByTime(1000);
    expect(FakeWebSocket.instances).toHaveLength(3);
  });

  it('dispose() closes, cancels timers, drops listeners, refuses reuse', async () => {
    const c = new RBGridWebSocketClient({ url: 'h', port: 80 });
    const seen: unknown[] = [];
    c.on(WebSocketEvent.MESSAGE, (m) => seen.push(m));
    c.connect('/ws-telemetry');
    FakeWebSocket.instances[0].fail(); // reconnect timer pending
    c.dispose();
    jest.advanceTimersByTime(60000);
    expect(FakeWebSocket.instances).toHaveLength(1);
    await expect(c.connect()).rejects.toThrow('disposed');
    expect(jest.getTimerCount()).toBe(0);
  });

  it('outgoing buffer is bounded (oldest dropped)', () => {
    const c = new RBGridWebSocketClient({ url: 'h', port: 80, bufferOptions: { maxSize: 3 } });
    for (let i = 0; i < 10; i++) c.send('x', i);
    expect(c.getBufferedMessageCount()).toBe(3);
    c.connect('/ws-telemetry');
    FakeWebSocket.instances[0].open();
    expect(FakeWebSocket.instances[0].sent.map((s) => JSON.parse(s).data)).toEqual([7, 8, 9]);
    expect(c.getBufferedMessageCount()).toBe(0);
  });

  it('ignores events from a replaced socket', () => {
    const c = new RBGridWebSocketClient({ url: 'h', port: 80 });
    const seen: unknown[] = [];
    c.on(WebSocketEvent.MESSAGE, (m) => seen.push(m));
    c.connect('/ws-telemetry');
    const old = FakeWebSocket.instances[0];
    old.open();
    c.reconnect();
    old.message({ units: { A: 1 } }); // late frame from the old socket
    expect(seen).toHaveLength(0);
  });
});

describe('client cache — B-6 device switch', () => {
  it('device switch disposes the old client and targets the new device', () => {
    connectDevice('192.168.1.139');
    const a = getTelemetryClient();
    a.connect('/ws-telemetry');
    expect(FakeWebSocket.instances[0].url).toBe('ws://192.168.1.139:80/ws-telemetry');
    FakeWebSocket.instances[0].open();

    connectDevice('192.168.1.50:8080');
    expect(FakeWebSocket.instances[0].closed).toBe(true);
    expect(a.isDisposed()).toBe(true);

    const b = getTelemetryClient();
    expect(b).not.toBe(a);
    b.connect('/ws-telemetry');
    expect(FakeWebSocket.instances[1].url).toBe('ws://192.168.1.50:8080/ws-telemetry');
    jest.advanceTimersByTime(60000); // b (never opened) keeps retrying; a must not
    expect(FakeWebSocket.instances.filter((w) => w.url.includes('192.168.1.139'))).toHaveLength(1);
  });

  it('disconnect() tears down all clients', () => {
    connectDevice('192.168.1.139');
    const a = getTelemetryClient();
    a.connect('/ws-telemetry');
    disconnect();
    expect(a.isDisposed()).toBe(true);
    expect(FakeWebSocket.instances[0].closed).toBe(true);
  });

  it('same device → same cached client', () => {
    connectDevice('192.168.1.139');
    expect(getTelemetryClient()).toBe(getTelemetryClient());
  });
});
