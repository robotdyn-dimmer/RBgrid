import { CmdChannel, MqttLike, CmdTimeoutError } from './CmdChannel';

/** Controllable in-memory broker: capture publishes, deliver responses on demand. */
class FakeMqtt implements MqttLike {
  published: { topic: string; payload: string }[] = [];
  private handlers = new Map<string, Set<(t: string, p: string) => void>>();
  publish(topic: string, payload: string) { this.published.push({ topic, payload }); }
  subscribe(topic: string, handler: (t: string, p: string) => void) {
    if (!this.handlers.has(topic)) this.handlers.set(topic, new Set());
    this.handlers.get(topic)!.add(handler);
    return () => this.handlers.get(topic)!.delete(handler);
  }
  deliver(topic: string, payload: string) {
    this.handlers.get(topic)?.forEach((h) => h(topic, payload));
  }
  get lastCmd() { return JSON.parse(this.published[this.published.length - 1].payload); }
}

const ROOT = 'rbgrid/aabbcc';

describe('CmdChannel', () => {
  it('publishes a cmd with a unique id and resolves on the id-correlated response', async () => {
    const mqtt = new FakeMqtt();
    const ch = new CmdChannel(mqtt, ROOT);
    const p = ch.get('tariff');

    expect(mqtt.published[0].topic).toBe('rbgrid/aabbcc/cmd');
    const sent = mqtt.lastCmd;
    expect(sent.cmd).toBe('get');
    expect(sent.section).toBe('tariff');
    expect(sent.id).toBeTruthy();

    mqtt.deliver('rbgrid/aabbcc/cmd/resp', JSON.stringify({ id: sent.id, type: 'tariff', data: { rate: 0.2 } }));
    const res = await p;
    expect(res.data).toEqual({ rate: 0.2 });
  });

  it('ignores responses with a non-matching id, resolves on the matching one', async () => {
    const mqtt = new FakeMqtt();
    const ch = new CmdChannel(mqtt, ROOT);
    const p = ch.request('get', { section: 'system' });
    const id = mqtt.lastCmd.id;

    mqtt.deliver('rbgrid/aabbcc/cmd/resp', JSON.stringify({ id: 'someone-else', data: { wrong: true } }));
    mqtt.deliver('rbgrid/aabbcc/cmd/resp', JSON.stringify({ id, data: { ok: true } }));
    expect((await p).data).toEqual({ ok: true });
  });

  it('ignores non-JSON traffic on the resp topic', async () => {
    const mqtt = new FakeMqtt();
    const ch = new CmdChannel(mqtt, ROOT);
    const p = ch.get('wifi');
    const id = mqtt.lastCmd.id;
    mqtt.deliver('rbgrid/aabbcc/cmd/resp', 'not json');
    mqtt.deliver('rbgrid/aabbcc/cmd/resp', JSON.stringify({ id, data: 1 }));
    expect((await p).data).toBe(1);
  });

  it('gives each request a distinct id (concurrent on shared resp topic)', async () => {
    const mqtt = new FakeMqtt();
    const ch = new CmdChannel(mqtt, ROOT);
    const a = ch.get('system');
    const idA = mqtt.lastCmd.id;
    const b = ch.get('units');
    const idB = mqtt.lastCmd.id;
    expect(idA).not.toBe(idB);
    mqtt.deliver('rbgrid/aabbcc/cmd/resp', JSON.stringify({ id: idB, data: 'B' }));
    mqtt.deliver('rbgrid/aabbcc/cmd/resp', JSON.stringify({ id: idA, data: 'A' }));
    expect((await a).data).toBe('A');
    expect((await b).data).toBe('B');
  });

  it('rejects with CmdTimeoutError when no response arrives', async () => {
    const mqtt = new FakeMqtt();
    // fire the timeout immediately
    const ch = new CmdChannel(mqtt, ROOT, { setTimeoutFn: (fn) => { fn(); return 0; }, clearTimeoutFn: () => {} });
    await expect(ch.get('tariff')).rejects.toBeInstanceOf(CmdTimeoutError);
  });

  it('set sends the full envelope as data (SET round-trip gotcha)', async () => {
    const mqtt = new FakeMqtt();
    const ch = new CmdChannel(mqtt, ROOT);
    const envelope = { type: 'tariff', version: 1, device_id: 'x', data: { rate: 0.27 } };
    const p = ch.set('tariff', envelope);
    const sent = mqtt.lastCmd;
    expect(sent.cmd).toBe('set');
    expect(sent.section).toBe('tariff');
    expect(sent.data).toEqual(envelope); // whole envelope, not just inner data
    mqtt.deliver('rbgrid/aabbcc/cmd/resp', JSON.stringify({ id: sent.id, type: 'cmd_ack', data: { result: 'ok' } }));
    expect((await p).data).toEqual({ result: 'ok' });
  });
});
