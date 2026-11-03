/**
 * BrowserMqttClient — the concrete MqttLike over mqtt.js (MQTT-over-WebSocket).
 * The browser can't speak raw MQTT TCP, so this connects to the broker's WS listener
 * (`wss://` for a TLS broker, or plain `ws://` for a LAN broker — the bench broker at
 * ws://192.168.1.100:9001 is plain ws; live-validated against the real device 2026-10-11).
 * Wired by the TransportRouter for MQTT-mode targets once infra provides the ws coords.
 *
 * mqtt.js is ~19 % of the bundle and only broker mode needs it, so it is loaded LAZILY
 * (its own chunk). Calls made before it arrives are queued and replayed on connect —
 * the MqttLike surface stays synchronous for callers.
 */
import type { MqttClient } from 'mqtt';
import { MqttLike } from './CmdChannel';

export interface BrokerOptions {
  /** Broker WebSocket URL — "ws://host:9001" (LAN) or "wss://broker.example:8884/mqtt" (TLS). */
  url: string;
  username?: string;
  password?: string;
  clientId?: string;
}

export class BrowserMqttClient implements MqttLike {
  private client: MqttClient | null = null;
  private readonly pending: ((c: MqttClient) => void)[] = [];
  private ended = false;
  private readonly handlers = new Map<string, Set<(t: string, p: string) => void>>();

  constructor(opts: BrokerOptions) {
    void import(/* webpackChunkName: "mqtt" */ 'mqtt').then((mod) => {
      if (this.ended) return;
      // CJS/ESM interop: `connect` is a named export, or lives on `default` under webpack.
      type Connect = typeof import('mqtt').connect;
      const m = mod as unknown as { connect?: Connect; default?: { connect?: Connect } };
      const connect = (m.connect ?? m.default?.connect) as Connect;
      const client = connect(opts.url, {
        username: opts.username,
        password: opts.password,
        clientId: opts.clientId,
        protocolVersion: 5,
        reconnectPeriod: 3000,
      });
      client.on('message', (topic: string, payload: Uint8Array) => {
        const text = payload.toString();
        this.handlers.get(topic)?.forEach((h) => h(topic, text));
      });
      this.client = client;
      this.pending.splice(0).forEach((f) => f(client));
    });
  }

  /** Run now if connected, else queue until the lazily-loaded client exists. */
  private withClient(f: (c: MqttClient) => void): void {
    if (this.client) f(this.client);
    else if (!this.ended) this.pending.push(f);
  }

  publish(topic: string, payload: string): void {
    this.withClient((c) => c.publish(topic, payload));
  }

  subscribe(topic: string, handler: (t: string, p: string) => void): () => void {
    if (!this.handlers.has(topic)) {
      this.handlers.set(topic, new Set());
      this.withClient((c) => c.subscribe(topic));
    }
    this.handlers.get(topic)!.add(handler);
    return () => {
      const set = this.handlers.get(topic);
      if (!set) return;
      set.delete(handler);
      if (set.size === 0) {
        this.handlers.delete(topic);
        this.withClient((c) => c.unsubscribe(topic));
      }
    };
  }

  end(): void {
    this.ended = true;
    this.pending.length = 0;
    this.client?.end();
  }
}
