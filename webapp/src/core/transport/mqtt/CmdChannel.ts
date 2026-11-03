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
  publish(topic: string, payload: string): void;
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

export class CmdChannel {
  private seq = 0;
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
    return `c${++this.seq}`;
  }

  /** Issue a command and resolve with the id-correlated response (or reject on timeout). */
  request(cmd: string, extra: Record<string, unknown> = {}): Promise<CmdResponse> {
    const id = this.nextId();
    const reqTopic = `${this.topicRoot}/cmd`;
    const respTopic = `${this.topicRoot}/cmd/resp`;

    return new Promise<CmdResponse>((resolve, reject) => {
      let settled = false;
      let unsubscribe = () => {};
      const timer = this.setTimeoutFn(() => {
        if (settled) return;
        settled = true;
        unsubscribe();
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
        resolve(msg);
      });

      this.mqtt.publish(reqTopic, JSON.stringify({ id, cmd, ...extra }));
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
