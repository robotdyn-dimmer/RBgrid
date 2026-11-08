/**
 * CmdChannel — the device's MQTT command channel (request/response with id-echo
 * correlation), confirmed by root. Publishes to `rbgrid/<mac>/cmd` and resolves on the
 * matching `rbgrid/<mac>/cmd/resp` whose top-level `id` echoes the request.
 *
 * Abstracted over a minimal `MqttLike` pub/sub so the correlation + timeout logic is
 * unit-testable without a live broker (the concrete mqtt.js adapter implements MqttLike).
 *
 * NOTE: the broker currently exposes one shared cmd/resp topic (per-client response
 * topic is a confirmed firmware/broker gap) — correlation is by `id`, so concurrent
 * requests from one client are fine; a per-client topic would further isolate N clients.
 */

export interface MqttLike {
  /** Publish; may return a cancel for a message still queued (not yet on the wire). */
  publish(topic: string, payload: string): (() => void) | void;
  /** Subscribe to a topic; returns an unsubscribe function. */
  subscribe(topic: string, handler: (topic: string, payload: string) => void): () => void;
}

export interface CmdResponse {
  id?: string;
  type?: string;
  status?: string;
  data?: unknown;
  [k: string]: unknown;
}

export interface CmdChannelOptions {
  timeoutMs?: number;
  /** Injectable scheduler for tests. */
  setTimeoutFn?: (fn: () => void, ms: number) => unknown;
  clearTimeoutFn?: (handle: unknown) => void;
}

export class CmdTimeoutError extends Error {
  constructor(cmd: string) {
    super(`MQTT cmd "${cmd}" timed out`);
    this.name = 'CmdTimeoutError';
  }
}

/** The hub answered with a `cmd_ack` whose `data.result` is "error" (unknown section, bad doc,
 *  admin token required…) — a refusal, not data (Kimi #9). */
export class CmdRejectedError extends Error {
  constructor(cmd: string, readonly reason: string, /** the ack's `data` (e.g. espnow's `reply`) */ readonly detail?: unknown) {
    super(`MQTT cmd "${cmd}" refused: ${reason}`);
    this.name = 'CmdRejectedError';
  }
}

/** A random tag per channel: the resp topic is shared by every client of the hub, so `c1` from two
 *  browsers matched each other's answers (Kimi #11). */
function instanceTag(): string {
  const c = (globalThis as { crypto?: { getRandomValues?: (a: Uint32Array) => Uint32Array } }).crypto;
  const a = new Uint32Array(2);
  if (c?.getRandomValues) c.getRandomValues(a);
  else { a[0] = Math.floor(Math.random() * 2 ** 32); a[1] = Math.floor(Math.random() * 2 ** 32); }
  return Array.from(a, (x) => x.toString(36)).join('');
}

export class CmdChannel {
  private seq = 0;
  private readonly tag = instanceTag();
  private readonly timeoutMs: number;
  private readonly setTimeoutFn: (fn: () => void, ms: number) => unknown;
  private readonly clearTimeoutFn: (handle: unknown) => void;

  constructor(
    private readonly mqtt: MqttLike,
    /** e.g. "rbgrid/aabbccddeeff" */
    private readonly topicRoot: string,
    opts: CmdChannelOptions = {},
  ) {
    this.timeoutMs = opts.timeoutMs ?? 6000;
    this.setTimeoutFn = opts.setTimeoutFn ?? ((fn, ms) => setTimeout(fn, ms));
    this.clearTimeoutFn = opts.clearTimeoutFn ?? ((h) => clearTimeout(h as ReturnType<typeof setTimeout>));
  }

  private nextId(): string {
    return `${this.tag}-${++this.seq}`;
  }

  /** Issue a command and resolve with the id-correlated response (or reject on timeout). */
  request(cmd: string, extra: Record<string, unknown> = {}): Promise<CmdResponse> {
    const id = this.nextId();
    const reqTopic = `${this.topicRoot}/cmd`;
    const respTopic = `${this.topicRoot}/cmd/resp`;

    return new Promise<CmdResponse>((resolve, reject) => {
      let settled = false;
      let unsubscribe = () => {};
      let cancelPublish: (() => void) | void;
      const timer = this.setTimeoutFn(() => {
        if (settled) return;
        settled = true;
        unsubscribe();
        // A command still queued (the broker client not loaded yet) must not go out after the UI
        // reported the timeout — a late SET the user believes failed (Kimi #10).
        if (typeof cancelPublish === 'function') cancelPublish();
        reject(new CmdTimeoutError(cmd));
      }, this.timeoutMs);

      unsubscribe = this.mqtt.subscribe(respTopic, (_topic, payload) => {
        if (settled) return;
        let msg: CmdResponse;
        try {
          msg = JSON.parse(payload) as CmdResponse;
        } catch {
          return; // ignore non-JSON / unrelated traffic
        }
        if (msg.id !== id) return; // correlation: only our echoed id
        settled = true;
        this.clearTimeoutFn(timer);
        unsubscribe();
        const ack = msg.type === 'cmd_ack' ? (msg.data as { result?: string; message?: string } | undefined) : undefined;
        if (ack?.result === 'error') reject(new CmdRejectedError(cmd, ack.message ?? 'error', ack));
        else resolve(msg);
      });

      cancelPublish = this.mqtt.publish(reqTopic, JSON.stringify({ id, cmd, ...extra }));
    });
  }

  /** GET a config section → the full enveloped response (per the SET round-trip gotcha). */
  get(section: string): Promise<CmdResponse> {
    return this.request('get', { section });
  }

  /** SET a config section. `data` MUST be the FULL envelope a prior get returned. */
  set(section: string, data: unknown): Promise<CmdResponse> {
    return this.request('set', { section, data });
  }
}
