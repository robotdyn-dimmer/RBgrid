import { MqttTransport } from './MqttTransport';
import { CmdChannel, MqttLike } from './mqtt/CmdChannel';

/**
 * Auto-responding fake broker: on each published cmd, echoes the id back on cmd/resp
 * with a canned body per section — exercises MqttTransport end-to-end without mqtt.js.
 */
class AutoBroker implements MqttLike {
  lastSet: { section: string; data: unknown } | null = null;
  private handlers = new Map<string, Set<(t: string, p: string) => void>>();
  private bodies: Record<string, unknown>;

  constructor(bodies: Record<string, unknown>) { this.bodies = bodies; }

  publish(topic: string, payload: string): void {
    const msg = JSON.parse(payload);
    const respTopic = topic.replace(/\/cmd$/, '/cmd/resp');
    let data: unknown = this.bodies[msg.section] ?? {};
    if (msg.cmd === 'set') {
      this.lastSet = { section: msg.section, data: msg.data };
      data = { result: 'ok' };
    }
    // deliver asynchronously to mimic a broker round-trip
    Promise.resolve().then(() =>
      this.deliver(respTopic, JSON.stringify({ id: msg.id, type: msg.section, data })),
    );
  }
  subscribe(topic: string, handler: (t: string, p: string) => void) {
    if (!this.handlers.has(topic)) this.handlers.set(topic, new Set());
    this.handlers.get(topic)!.add(handler);
    return () => this.handlers.get(topic)!.delete(handler);
  }
  private deliver(topic: string, payload: string) {
    this.handlers.get(topic)?.forEach((h) => h(topic, payload));
  }
}

const build = (bodies: Record<string, unknown>) => {
  const broker = new AutoBroker(bodies);
  const channel = new CmdChannel(broker, 'rbgrid/aabbcc');
  return { broker, t: new MqttTransport(channel) };
};

describe('MqttTransport (over the cmd channel)', () => {
  it('getSchema unwraps the enveloped cmd response', async () => {
    const { t } = build({ schema: { api_version: 1, telemetry_profile: 'lean' } });
    const schema = await t.getSchema();
    expect(schema?.api_version).toBe(1);
  });

  it('getConfig returns the inner config for a supported section', async () => {
    const { t } = build({ tariff: { rate: 0.2, name: 'Plan' } });
    expect(await t.getConfig('tariff')).toEqual({ rate: 0.2, name: 'Plan' });
  });

  it('setConfig round-trips the FULL envelope with the new inner data', async () => {
    const { broker, t } = build({ tariff: { rate: 0.2 } });
    await t.setConfig('tariff', { rate: 0.27 });
    // the set payload's data is the whole envelope, inner data replaced
    expect((broker.lastSet!.data as { data: unknown }).data).toEqual({ rate: 0.27 });
  });

  it('rejects unsupported config domains (not a cmd section)', async () => {
    const { t } = build({});
    await expect(t.getConfig('mqtt')).rejects.toThrow(/not available over the mqtt/);
    await expect(t.setConfig('system', {})).rejects.toThrow(/not available over the mqtt/);
  });

  it('reads economics + summary + espnow over cmd', async () => {
    const { t } = build({
      economics: { v: 1, unit: '9020', currency: 'USD', zones: [] },
      summary: { v: 1, unit: '9020', period: 'day' },
      espnow: { running: true, nodes: [] },
    });
    expect((await t.getEconomics('9020'))?.currency).toBe('USD');
    expect((await t.getSummary('9020', 'day'))?.period).toBe('day');
    expect((await t.getEspnow())?.running).toBe(true);
  });

  it('keeps non-cmd ops Direct-only (throws)', async () => {
    const { t } = build({});
    await expect(t.setToken('x')).rejects.toThrow(/mqtt/);
    await expect(t.exportCsv()).rejects.toThrow(/HTTP-only/);
    await expect(t.getProvisioning()).resolves.toBeNull();
  });
});
