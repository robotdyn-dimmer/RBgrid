/**
 * DeviceSession — the device-management orchestrator for ONE device.
 *
 * Holds the active transport, the discovered capabilities, the config DeviceShadow,
 * and the JobTracker, and drives the reconciliation loop:
 *   loadDomain (GET → reported) → edit (desired) → applyDomain (delta → setConfig →
 *   reload reported; reboot-domains await reconnect instead of reloading).
 *
 * Non-React + injectable clock/transport so the whole reconciliation flow is unit-
 * testable with a fake transport. The React layer (DeviceProvider) subscribes via
 * `onChange` and re-renders.
 */
import { JsonObject } from '../json';
import {
  DeviceCapabilities, BASELINE_CAPABILITIES, deriveCapabilities, domainRequiresReboot, domainRebootFields,
} from '../schema/capabilities';
import { ConfigDomain, ConfigShadow, DeviceShadow, REBOOT_FIELDS_FALLBACK, REBOOT_DOMAINS } from '../shadow/ConfigShadow';
import { DeviceTransport } from '../transport/types';
import { ProvisioningStatus } from '../provisioning/ProvisioningFSM';
import { JobTracker, Job } from '../jobs/JobTracker';

export interface DeviceSessionOptions {
  transport: DeviceTransport;
  /** Injectable clock (ms epoch). Defaults to Date.now in the app. */
  clock?: () => number;
  /** Called whenever session state changes (for React re-render). */
  onChange?: () => void;
}

export class DeviceSession {
  readonly transport: DeviceTransport;
  readonly jobs: JobTracker;
  private readonly clock: () => number;
  private readonly onChange?: () => void;

  capabilities: DeviceCapabilities = BASELINE_CAPABILITIES;
  shadow: DeviceShadow = new DeviceShadow();
  /** Last fetched onboarding status (null until loaded / unsupported). */
  provisioning: ProvisioningStatus | null = null;
  /** True once the schema fetch has been attempted. */
  initialized = false;

  constructor(opts: DeviceSessionOptions) {
    this.transport = opts.transport;
    this.clock = opts.clock ?? (() => Date.now());
    this.onChange = opts.onChange;
    this.jobs = new JobTracker(this.clock, this.onChange);
  }

  private notify() {
    this.onChange?.();
  }

  /** Discover capabilities from the schema (null → baseline). */
  async init(): Promise<DeviceCapabilities> {
    const schema = await this.transport.getSchema();
    this.capabilities = deriveCapabilities(schema);
    this.initialized = true;
    this.notify();
    return this.capabilities;
  }

  /** Pull the onboarding status (drives the provisioning wizard / setup view). */
  async loadProvisioning(): Promise<ProvisioningStatus | null> {
    this.provisioning = await this.transport.getProvisioning();
    this.notify();
    return this.provisioning;
  }

  /** One GET per domain in flight: every reader that asks while it runs shares it. */
  private loading = new Map<ConfigDomain, Promise<void>>();

  /** Pull a domain's reported config into the shadow (preserving any pending edits).
   *  Concurrent calls share ONE request (3φ audit 16.10 H3P-03: ~45 GETs of time-config in 25 s
   *  — every component with the hub's clock asked while the first was still running, the hub
   *  choked and the rest timed out). */
  loadDomain(domain: ConfigDomain): Promise<void> {
    const running = this.loading.get(domain);
    if (running) return running;
    const p = (async () => {
      try {
        const reported = await this.transport.getConfig(domain);
        const next = this.shadow.get(domain).withReported(reported);
        this.shadow = this.shadow.set(domain, next);
        this.notify();
      } finally {
        this.loading.delete(domain);
      }
    })();
    this.loading.set(domain, p);
    return p;
  }

  /** Edit one desired key. */
  edit(domain: ConfigDomain, key: string, value: JsonObject[string]): void {
    const next = this.shadow.get(domain).withDesired(key, value, this.clock());
    this.shadow = this.shadow.set(domain, next);
    this.notify();
  }

  /** Replace a domain's whole desired document (form-level edit). */
  editDoc(domain: ConfigDomain, doc: JsonObject): void {
    const next = this.shadow.get(domain).withDesiredDoc(doc, this.clock());
    this.shadow = this.shadow.set(domain, next);
    this.notify();
  }

  /** Drop pending edits for a domain. */
  resetDomain(domain: ConfigDomain): void {
    this.shadow = this.shadow.set(domain, this.shadow.get(domain).reset());
    this.notify();
  }

  get current(): DeviceShadow {
    return this.shadow;
  }

  /**
   * Will applying this domain's pending changes require a reboot? Device-driven: the
   * schema's per-endpoint `requires_reboot` (landed 2026-06-23) wins; falls back to the
   * static REBOOT_DOMAINS heuristic when the schema doesn't cover the domain.
   */
  domainNeedsReboot(domain: ConfigDomain): boolean {
    const shadow = this.shadow.get(domain);
    const fromSchema = domainRequiresReboot(this.capabilities, domain);
    if (fromSchema === undefined) return shadow.requiresReboot;
    if (!shadow.hasChanges || !fromSchema) return false;
    return Object.keys(shadow.delta).some((k) => this.fieldNeedsReboot(domain, k));
  }

  /**
   * Does this one setting take effect only after a hub restart? Same sources as
   * domainNeedsReboot, per top-level key — lets a page set start-time fields apart from live ones.
   */
  fieldNeedsReboot(domain: ConfigDomain, key: string): boolean {
    const fromSchema = domainRequiresReboot(this.capabilities, domain);
    if (fromSchema === undefined) return REBOOT_DOMAINS.has(domain);
    if (!fromSchema) return false;
    // A mixed endpoint names its on-start fields (schema reboot_fields, else root's list).
    const list = domainRebootFields(this.capabilities, domain) ?? REBOOT_FIELDS_FALLBACK[domain];
    return list ? list.includes(key) : true;
  }

  /** Pending edits split by how they take effect: `live` (at once) / `reboot` (after a restart). */
  pendingSplit(): { live: ConfigDomain[]; reboot: ConfigDomain[] } {
    const live: ConfigDomain[] = [];
    const reboot: ConfigDomain[] = [];
    for (const d of this.shadow.dirtyDomains) {
      const keys = Object.keys(this.shadow.get(d).delta);
      if (keys.some((k) => !this.fieldNeedsReboot(d, k))) live.push(d);
      if (keys.some((k) => this.fieldNeedsReboot(d, k))) reboot.push(d);
    }
    return { live, reboot };
  }

  /**
   * Write only the edits that take effect at once. A domain mixing both kinds (system:
   * language next to logging) sends its live keys on top of the REPORTED doc, so the
   * start-time edits stay pending for «Сохранить и перезагрузить».
   */
  async applyLive(): Promise<Job[]> {
    const jobs: Job[] = [];
    const failures: { domain: ConfigDomain; error: unknown }[] = [];
    for (const domain of this.pendingSplit().live) {
      try {
        const keys = Object.keys(this.shadow.get(domain).delta);
        const liveKeys = keys.filter((k) => !this.fieldNeedsReboot(domain, k));
        const job = liveKeys.length === keys.length ? await this.applyDomain(domain) : await this.applyKeys(domain, liveKeys);
        if (job) jobs.push(job);
      } catch (error) {
        failures.push({ domain, error });
      }
    }
    if (failures.length) throw new ApplyAllError(failures, jobs);
    return jobs;
  }

  /** Write the edits that need a restart (their whole domains); the jobs await the reboot. */
  async applyRebootPart(): Promise<Job[]> {
    const jobs: Job[] = [];
    const failures: { domain: ConfigDomain; error: unknown }[] = [];
    for (const domain of this.pendingSplit().reboot) {
      try {
        const job = await this.applyDomain(domain);
        if (job) jobs.push(job);
      } catch (error) {
        failures.push({ domain, error });
      }
    }
    if (failures.length) throw new ApplyAllError(failures, jobs);
    return jobs;
  }

  /** Push some top-level keys of desired over reported; the rest of desired stays pending. */
  private async applyKeys(domain: ConfigDomain, keys: string[]): Promise<Job> {
    const before = this.shadow.get(domain);
    const doc: JsonObject = { ...before.reported };
    for (const k of keys) doc[k] = before.desired[k];
    const job = this.jobs.start('config-push', { domain, field: keys.join(',') });
    try {
      await this.transport.setConfig(domain, doc);
      const reported = await this.transport.getConfig(domain);
      const cur = this.shadow.get(domain);
      const desired: JsonObject = { ...cur.desired };
      for (const k of keys) if (k in reported) desired[k] = reported[k];
      this.shadow = this.shadow.set(domain, new ConfigShadow(domain, reported, desired, cur.version + 1, cur.metadata));
      this.jobs.resolve(job.id);
      this.notify();
      return job;
    } catch (e) {
      this.jobs.fail(job.id, e instanceof Error ? e.message : String(e));
      throw e;
    }
  }

  /**
   * Reconcile one domain: push the desired doc, then converge.
   * Reboot-domains can't be reloaded immediately (device restarts) → the job is left
   * `awaiting-reboot` for the reconnect flow to resolve. Non-reboot domains reload
   * reported so the delta empties.
   */
  async applyDomain(domain: ConfigDomain): Promise<Job | null> {
    const shadow = this.shadow.get(domain);
    if (!shadow.hasChanges) return null;

    const job = this.jobs.start('config-push', { domain });
    try {
      const expected = this.domainNeedsReboot(domain);
      const res: { requiresReboot?: boolean } = (await this.transport.setConfig(domain, shadow.desired)) || {};
      // The device's answer to this write wins over the schema / field-list guess.
      const reboot = typeof res.requiresReboot === 'boolean' ? res.requiresReboot : expected;
      if (reboot) {
        this.jobs.awaitReboot(job.id);
      } else {
        await this.loadDomain(domain);
        this.jobs.resolve(job.id);
      }
      return job;
    } catch (e) {
      this.jobs.fail(job.id, e instanceof Error ? e.message : String(e));
      throw e;
    }
  }

  /**
   * Live-apply ONE field without touching the rest of the user's pending edits and without the
   * reboot path (review 02 B-8/B-9; spec §13 G1: site_profile/ev live-apply, the device's
   * requires_reboot flag is ignored for them). Base = the device's REPORTED doc (+ this field),
   * so unrelated desired edits are never sent along; after the POST the domain is re-read and
   * the field's desired value is aligned to reported (no perpetual "dirty").
   */
  async applyLiveField(domain: ConfigDomain, key: string, value: JsonObject[string]): Promise<void> {
    if (Object.keys(this.shadow.get(domain).reported).length === 0) await this.loadDomain(domain);
    const before = this.shadow.get(domain);
    const doc: JsonObject = { ...before.reported, [key]: value };
    const job = this.jobs.start('config-push', { domain, field: key });
    try {
      await this.transport.setConfig(domain, doc);
      const reported = await this.transport.getConfig(domain);
      const cur = this.shadow.get(domain);
      // Keep other pending edits; the live field follows the device.
      const desired = { ...cur.desired, [key]: reported[key] ?? value };
      this.shadow = this.shadow.set(domain, new ConfigShadow(domain, reported, desired, cur.version + 1, cur.metadata));
      this.jobs.resolve(job.id);
      this.notify();
    } catch (e) {
      this.jobs.fail(job.id, e instanceof Error ? e.message : String((e as { message?: string })?.message ?? e));
      throw e;
    }
  }

  /**
   * Patch one unit. Prefers the per-unit endpoint; on firmware without it (404/405) falls back
   * to the full units array (`fullUnits` = the reported list with this unit already patched).
   * Either way the units domain is re-read so every screen follows the device.
   */
  async patchUnit(
    hexId: string, patch: JsonObject, fullUnits: JsonObject[], beforeFallback?: () => void,
  ): Promise<void> {
    if (this.transport.patchUnit) {
      try {
        await this.transport.patchUnit(hexId, patch);
        await this.loadDomain('units');
        return;
      } catch (e) {
        const status = (e as { status?: number }).status;
        if (status !== 404 && status !== 405) throw e;
      }
    }
    beforeFallback?.();
    await this.applyLiveField('units', 'units', fullUnits as unknown as JsonObject[string]);
  }

  /**
   * Reconcile every dirty domain — each on its own: one refused domain no longer stops the rest
   * (wizard on a fresh hub 17.10, W191-02: a read-only «time» edit blocked the site name too).
   * Throws an ApplyAllError naming every domain that didn't apply, after trying them all.
   */
  /**
   * Close the jobs left `awaiting-reboot` once the hub is back (Kimi #14: they stayed active and
   * the delta «written, applies after a reboot» never cleared). Waits `graceMs` for the restart to
   * begin, polls until a config read answers (≤ `waitMs`), re-reads each domain and resolves the
   * job when its delta emptied — fails it when the hub kept another value or never came back.
   */
  async reconcileAfterReboot(opts: { graceMs?: number; pollMs?: number; waitMs?: number; sleep?: (ms: number) => Promise<void> } = {}): Promise<{ applied: ConfigDomain[]; notApplied: ConfigDomain[] }> {
    const sleep = opts.sleep ?? ((ms: number) => new Promise<void>((r) => setTimeout(r, ms)));
    const pending = this.jobs.jobs.filter((j) => j.status === 'awaiting-reboot' && j.detail?.domain);
    const domains = Array.from(new Set(pending.map((j) => j.detail!.domain as ConfigDomain)));
    if (!domains.length) return { applied: [], notApplied: [] };
    await sleep(opts.graceMs ?? 8000);
    const deadline = this.clock() + (opts.waitMs ?? 120_000);
    let back = false;
    while (!back) {
      try { await this.loadDomain(domains[0]); back = true; } catch {
        if (this.clock() >= deadline) break;
        await sleep(opts.pollMs ?? 3000);
      }
    }
    const applied: ConfigDomain[] = [];
    const notApplied: ConfigDomain[] = [];
    for (const d of domains) {
      let ok = back;
      if (ok && d !== domains[0]) { try { await this.loadDomain(d); } catch { ok = false; } }
      ok = ok && !this.shadow.get(d).hasChanges;
      (ok ? applied : notApplied).push(d);
      for (const j of pending) {
        if (j.detail?.domain !== d) continue;
        if (ok) this.jobs.resolve(j.id);
        else this.jobs.fail(j.id, back ? 'not applied after the restart' : 'the hub did not come back');
      }
    }
    this.notify();
    return { applied, notApplied };
  }

  /** Did any of these (just applied) jobs end up waiting for a reboot? */
  awaitingReboot(applied: readonly Job[]): boolean {
    const ids = new Set(applied.map((j) => j.id));
    return this.jobs.jobs.some((j) => ids.has(j.id) && j.status === 'awaiting-reboot');
  }

  async applyAll(): Promise<Job[]> {
    const jobs: Job[] = [];
    const failures: { domain: ConfigDomain; error: unknown }[] = [];
    for (const domain of this.shadow.dirtyDomains) {
      try {
        const job = await this.applyDomain(domain);
        if (job) jobs.push(job);
      } catch (error) {
        failures.push({ domain, error });
      }
    }
    if (failures.length) throw new ApplyAllError(failures, jobs);
    return jobs;
  }
}

/** Some dirty domains didn't apply; the others did (`applied`). */
export class ApplyAllError extends Error {
  constructor(public readonly failures: { domain: ConfigDomain; error: unknown }[], public readonly applied: Job[]) {
    super(`Not applied: ${failures.map((f) => f.domain).join(', ')}`);
    this.name = 'ApplyAllError';
  }
}
