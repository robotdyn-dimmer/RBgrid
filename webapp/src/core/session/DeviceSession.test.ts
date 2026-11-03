import { DeviceSession } from './DeviceSession';
import { DeviceTransport, TransportKind } from '../transport/types';
import { ConfigDomain } from '../shadow/ConfigShadow';
import { JsonObject } from '../json';
import { RawSchema } from '../schema/capabilities';
import { ProvisioningStatus } from '../provisioning/ProvisioningFSM';
import { Economics, Summary } from '../economics/types';
import { mockSchema, mockProvisioning, mockEconomics, mockSummary } from '../mocks';

/** In-memory transport for testing the reconciliation flow. */
class FakeTransport implements DeviceTransport {
  readonly kind: TransportKind = 'direct';
  setCalls: { domain: ConfigDomain; doc: JsonObject }[] = [];
  constructor(private store: Partial<Record<ConfigDomain, JsonObject>> = {}) {}
  async getSchema(): Promise<RawSchema | null> {
    return mockSchema();
  }
  async getConfig(domain: ConfigDomain): Promise<JsonObject> {
    return JSON.parse(JSON.stringify(this.store[domain] ?? {}));
  }
  /** What the next writes answer (the hub's per-write requires_reboot); undefined = says nothing. */
  answer: { requiresReboot?: boolean } | undefined;
  async setConfig(domain: ConfigDomain, doc: JsonObject): Promise<{ requiresReboot?: boolean } | void> {
    this.setCalls.push({ domain, doc: JSON.parse(JSON.stringify(doc)) });
    this.store[domain] = JSON.parse(JSON.stringify(doc)); // device now reports the new state
    return this.answer;
  }
  async getProvisioning(): Promise<ProvisioningStatus | null> {
    return mockProvisioning();
  }
  async getEconomics(unit: string): Promise<Economics | null> {
    return mockEconomics(unit);
  }
  async getSummary(unit: string): Promise<Summary | null> {
    return mockSummary(unit);
  }
  async setTelemetryProfile(): Promise<void> { /* noop */ }
  async getTokenStatus(): Promise<boolean> { return false; }
  async getTokenRoles() { return { adminSet: false, readSet: false }; }
  async setToken(): Promise<void> { /* noop */ }
  async getSecurity() { return null; }
  async getAp() { return null; }
  async setApPassword() { return {}; }
  async setSecurity(): Promise<void> { /* noop */ }
  async getBackupStatus() { return null; }
  async createBackup() { return { filesBackedUp: 0 }; }
  async restoreBackup() { return { status: 'OK', filesRestored: 0, requiresReboot: true }; }
  async postOta() { return { status: 'ok', message: '', partition: 'ota_0' }; }
  async getEspnow() { return null; }
  async espnowAction() { return { ok: true }; }
  async exportCsv() { return null; }
}

const fixedClock = () => 1000;

describe('DeviceSession', () => {
  it('init derives capabilities from the schema', async () => {
    const s = new DeviceSession({ transport: new FakeTransport(), clock: fixedClock });
    const caps = await s.init();
    expect(caps.fromSchema).toBe(true);
    expect(caps.flags.economics).toBe(true);
    expect(caps.flags.espnow).toBe(true);
    expect(['lean', 'full']).toContain(caps.telemetryProfile);
  });

  it('loads reported config into the shadow', async () => {
    const t = new FakeTransport({ tariff: { rate: 0.2, name: 'Plan A' } });
    const s = new DeviceSession({ transport: t, clock: fixedClock });
    await s.loadDomain('tariff');
    expect(s.current.get('tariff').reported).toEqual({ rate: 0.2, name: 'Plan A' });
    expect(s.current.get('tariff').hasChanges).toBe(false);
  });

  it('reconciles a non-reboot domain: push → reload → delta empties → job succeeds', async () => {
    const t = new FakeTransport({ tariff: { rate: 0.2 } });
    const s = new DeviceSession({ transport: t, clock: fixedClock });
    await s.loadDomain('tariff');
    s.edit('tariff', 'rate', 0.27);
    expect(s.current.get('tariff').delta).toEqual({ rate: 0.27 });

    const job = await s.applyDomain('tariff');
    expect(t.setCalls).toHaveLength(1);
    expect(t.setCalls[0]).toEqual({ domain: 'tariff', doc: { rate: 0.27 } });
    expect(s.current.get('tariff').hasChanges).toBe(false); // converged after reload
    expect(s.current.get('tariff').reported).toEqual({ rate: 0.27 });
    expect(job && s.jobs.jobs.find((j) => j.id === job.id)?.status).toBe('succeeded');
  });

  it('reboot domain: push, then await reconnect (no immediate reload)', async () => {
    const t = new FakeTransport({ wifi: { ssid: 'a' } });
    const s = new DeviceSession({ transport: t, clock: fixedClock });
    await s.loadDomain('wifi');
    s.edit('wifi', 'ssid', 'b');

    const job = await s.applyDomain('wifi');
    expect(t.setCalls).toHaveLength(1);
    expect(job && s.jobs.jobs.find((j) => j.id === job.id)?.status).toBe('awaiting-reboot');
    // desired stays ahead of reported until the device comes back and we re-pull
    expect(s.current.get('wifi').desired).toEqual({ ssid: 'b' });
  });

  it('applyAll reconciles every dirty domain', async () => {
    const t = new FakeTransport({ tariff: { rate: 0.2 }, units: { count: 1 } });
    const s = new DeviceSession({ transport: t, clock: fixedClock });
    await s.loadDomain('tariff');
    await s.loadDomain('units');
    s.edit('tariff', 'rate', 0.3);
    s.edit('units', 'count', 2);
    const jobs = await s.applyAll();
    expect(jobs).toHaveLength(2);
    expect(s.current.hasChanges).toBe(false);
  });

  it('device-driven reboot: schema requires_reboot beats the static heuristic', async () => {
    // 'system' is NOT in the static REBOOT_DOMAINS, but the landed schema marks
    // /api/system/system-config requires_reboot:true → apply must await reboot.
    // A start-time field (logging) is what that flag is about.
    const t = new FakeTransport({ system: { home_name: 'Home', logging_level: 3 } });
    const s = new DeviceSession({ transport: t, clock: fixedClock });
    await s.init(); // loads mockSchema → capabilities carry per-endpoint flags
    await s.loadDomain('system');
    s.edit('system', 'logging_level', 4);
    expect(s.domainNeedsReboot('system')).toBe(true);
    const job = await s.applyDomain('system');
    expect(job && s.jobs.jobs.find((j) => j.id === job.id)?.status).toBe('awaiting-reboot');
    expect(job && s.awaitingReboot([job])).toBe(true);
  });

  it('system-config live fields (language, name) need no reboot despite the endpoint flag', async () => {
    const t = new FakeTransport({ system: { home_name: 'Home', language: 'RU' } });
    const s = new DeviceSession({ transport: t, clock: fixedClock });
    await s.init();
    await s.loadDomain('system');
    s.edit('system', 'language', 'EN');
    s.edit('system', 'home_name', 'Lab');
    expect(s.domainNeedsReboot('system')).toBe(false);
    const job = await s.applyDomain('system');
    expect(job && s.awaitingReboot([job])).toBe(false);
    expect(s.current.get('system').hasChanges).toBe(false);
  });

  it('splits pending edits: language applies now, logging waits for «save and restart»', async () => {
    const t = new FakeTransport({ system: { language: 'RU', logging_level: 3 }, hardware: { network: { nominal_voltage: 0 } } });
    const s = new DeviceSession({ transport: t, clock: fixedClock });
    await s.init();
    await s.loadDomain('system');
    await s.loadDomain('hardware');
    s.edit('system', 'language', 'EN');
    s.edit('system', 'logging_level', 4);
    s.edit('hardware', 'network', { nominal_voltage: 230 });
    expect(s.fieldNeedsReboot('system', 'language')).toBe(false);
    expect(s.fieldNeedsReboot('system', 'logging_level')).toBe(true);
    expect(s.pendingSplit()).toEqual({ live: ['system'], reboot: ['system', 'hardware'] });

    const live = await s.applyLive();
    expect(live).toHaveLength(1);
    // Only the language went out; logging stayed at the reported value in that write.
    expect(t.setCalls).toHaveLength(1);
    expect(t.setCalls[0].doc).toMatchObject({ language: 'EN', logging_level: 3 });
    expect(s.pendingSplit()).toEqual({ live: [], reboot: ['system', 'hardware'] });
    expect(s.current.get('system').desired.logging_level).toBe(4);

    const rest = await s.applyRebootPart();
    expect(rest).toHaveLength(2);
    expect(s.awaitingReboot(rest)).toBe(true);
    expect(t.setCalls[1].doc).toMatchObject({ language: 'EN', logging_level: 4 });
  });

  it("on-start fields: root's list by default, the schema's reboot_fields once the hub sends them", async () => {
    const t = new FakeTransport({ system: { logging_level: 3, enable_json_compression: false } });
    const s = new DeviceSession({ transport: t, clock: fixedClock });
    await s.init();
    expect(s.fieldNeedsReboot('system', 'enable_telemetry')).toBe(true);
    expect(s.fieldNeedsReboot('system', 'enable_json_compression')).toBe(false); // stored, unused
    expect(s.fieldNeedsReboot('system', 'language')).toBe(false);

    const schema = await mockSchema();
    const withList = {
      ...schema,
      endpoints: (schema?.endpoints ?? []).map((e) => (e.path === '/api/system/system-config'
        ? { ...e, requires_reboot: true, reboot_fields: ['language'] } : e)),
    };
    t.getSchema = async () => withList;
    await s.init();
    expect(s.fieldNeedsReboot('system', 'language')).toBe(true);
    expect(s.fieldNeedsReboot('system', 'logging_level')).toBe(false);
  });

  it("the hub's answer to the write wins over the guess", async () => {
    const t = new FakeTransport({ system: { logging_level: 3 } });
    const s = new DeviceSession({ transport: t, clock: fixedClock });
    await s.init();
    await s.loadDomain('system');
    s.edit('system', 'logging_level', 4);
    t.answer = { requiresReboot: false };
    const job = await s.applyDomain('system');
    expect(job && s.jobs.jobs.find((j) => j.id === job.id)?.status).toBe('succeeded');
  });

  it('device-driven reboot: schema requires_reboot:false avoids reboot (tariff reloads)', async () => {
    const t = new FakeTransport({ tariff: { rate: 0.2 } });
    const s = new DeviceSession({ transport: t, clock: fixedClock });
    await s.init();
    await s.loadDomain('tariff');
    s.edit('tariff', 'rate', 0.3);
    expect(s.domainNeedsReboot('tariff')).toBe(false);
    const job = await s.applyDomain('tariff');
    expect(job && s.jobs.jobs.find((j) => j.id === job.id)?.status).toBe('succeeded');
  });

  it('applyDomain is a no-op when there are no changes', async () => {
    const t = new FakeTransport({ tariff: { rate: 0.2 } });
    const s = new DeviceSession({ transport: t, clock: fixedClock });
    await s.loadDomain('tariff');
    const job = await s.applyDomain('tariff');
    expect(job).toBeNull();
    expect(t.setCalls).toHaveLength(0);
  });
});

describe('loadDomain — one request in flight (3φ audit H3P-03)', () => {
  it('concurrent readers share one GET; a later call fetches again', async () => {
    const t = new FakeTransport({ time: { timezone: 'JST-9' } });
    let gets = 0;
    const orig = t.getConfig.bind(t);
    t.getConfig = async (d: ConfigDomain) => { gets += 1; return orig(d); };
    const s = new DeviceSession({ transport: t });
    await Promise.all(Array.from({ length: 45 }, () => s.loadDomain('time')));
    expect(gets).toBe(1);
    expect(s.shadow.get('time').reported).toEqual({ timezone: 'JST-9' });
    await s.loadDomain('time');
    expect(gets).toBe(2);
  });
  it('a failed request is not cached', async () => {
    const t = new FakeTransport({});
    let n = 0;
    t.getConfig = async () => { n += 1; if (n === 1) throw new Error('timeout'); return { ok: true }; };
    const s = new DeviceSession({ transport: t });
    await expect(Promise.all([s.loadDomain('time'), s.loadDomain('time')])).rejects.toThrow('timeout');
    await s.loadDomain('time');
    expect(n).toBe(2);
  });
});
