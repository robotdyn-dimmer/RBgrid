import { RBGridWebSocketClient, setWsAuthSource, disconnectAllClients } from './websocket';

class FakeWS {
  static OPEN = 1; static CONNECTING = 0; static CLOSED = 3; static CLOSING = 2;
  static all: FakeWS[] = [];
  readyState = 0; onopen: any = null; onclose: any = null; onerror: any = null; onmessage: any = null;
  constructor(public url: string) { FakeWS.all.push(this); }
  send() { /* */ } close() { this.readyState = 3; }
}

it('a hub restarted without FIN: the silence watchdog reconnects with a new ticket', async () => {
  (global as any).WebSocket = FakeWS;
  jest.useFakeTimers();
  let n = 0;
  setWsAuthSource(async () => ({ ticket: `t${++n}` }));
  const c = new RBGridWebSocketClient({ url: 'h', port: 80 });
  c.connect('/ws-telemetry').catch(() => undefined);
  for (let i = 0; i < 5; i++) await Promise.resolve();
  const ws = FakeWS.all[0];
  ws.readyState = 1; ws.onopen?.();
  ws.onmessage?.({ data: '{}' });
  // the hub goes silent
  for (let s = 0; s < 40; s++) { jest.advanceTimersByTime(1000); for (let i = 0; i < 5; i++) await Promise.resolve(); }
  expect(FakeWS.all.length).toBeGreaterThan(1);
  expect(FakeWS.all[1].url).toContain('ticket=t2');
  setWsAuthSource(null);
  disconnectAllClients();
  jest.useRealTimers();
});
