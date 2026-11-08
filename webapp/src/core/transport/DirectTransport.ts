/**
 * DirectTransport — the HTTP/HTTPS+WS backend for ONE device on the LAN (or same-origin).
 * Config GETs go through axios directly (+ extractConfigPayload); config POSTs go through
 * the shared api client (runtime base-URL, Bearer, typed errors, NO retry for POST).
 *
 * Config POST shapes — exactly what each firmware handler parses (rbwebserver.c /
 * rbwebserver_api.c, verified against fw 1.0.2; system verified live on .139 2026-10-13).
 * The shadow holds the INNER doc (extractConfigPayload); buildConfigPostBody re-wraps it.
 *
 * | domain    | endpoint                         | POST body                                         | notes |
 * |-----------|----------------------------------|---------------------------------------------------|-------|
 * | system    | /api/system/system-config        | {"system":{…inner}}                               | partial OK (absent = keep); else 400 MISSING_SYSTEM_SECTION; ≤2 KB; site_profile/ev applied live, resp still says requires_reboot:true |
 * | units     | /api/system/units-config         | {"type":"units_config","data":{"units":[…],…}}    | full list; ≤8 KB |
 * | tariff    | /api/system/tariff-plan-config   | {"type":"tariff_plan_list_config","data":{"list":[…]}} | saved verbatim; full build only (OSS: 501) |
 * | telemetry | /api/system/telemetry-config     | {"profile":"lean"\|"full"}                         | the ONLY writable field; <128 B; live |
 * | time      | /api/system/time-config          | — (GET-only route)                                | UnsupportedDomainError |
 * | wifi      | /api/system/wifi-config          | — (GET-only; creds via POST /wifi/save → reboot)  | UnsupportedDomainError |
 * | hardware  | /api/system/hardware-config      | {"type":"hardware_config","data":{network}} — GET round-trip (canon 2beb4bc8) | REAL reboot (nominals read at boot) |
 * | mqtt/espnow/security | —                     | —                                                 | UnsupportedDomainError |
 */
import axios from 'axios';
import { getApiClient } from '../../services/api';
import { getBaseUrl, isDemo, deviceSameOrigin } from '../../services/connection';
import { JsonObject } from '../json';
import { ConfigDomain } from '../shadow/ConfigShadow';
import { RawSchema } from '../schema/capabilities';
import { ProvisioningStatus } from '../provisioning/ProvisioningFSM';
import { Economics, Summary, SummaryPeriod } from '../economics/types';
import { EspnowStatus, EspnowAction, EspnowActionParams, EspnowActionResult } from '../espnow/types';
import { mockSchema, mockProvisioning, mockConfig, mockEconomics, mockSummary, mockEspnow } from '../mocks';
import { demoAdjustUnitsConfig, demoExportCsv } from '../../mock/demo';
import { tokenStore } from '../auth/authWiring';
import { extractConfigPayload } from './envelope';
import {
  DeviceTransport, TransportKind, UnsupportedDomainError, TokenRole, TokenRoles, SecurityConfig, ApStatus,
  BackupStatus, RestoreResult, OtaResult, OtaOptions, SetConfigResult,
} from './types';

/** Config-file domains → their device endpoint (the shadow's GET/POST targets). */
const CONFIG_DOMAIN_PATH: Partial<Record<ConfigDomain, string>> = {
  system: '/api/system/system-config',
  hardware: '/api/system/hardware-config',
  time: '/api/system/time-config',
  tariff: '/api/system/tariff-plan-config',
  units: '/api/system/units-config',
  mqtt: '/api/system/mqtt-config',
  telemetry: '/api/system/telemetry-config',
  wifi: '/api/system/wifi-config',
};

// The envelope parser is shared with the MQTT transport (one parser for both, Kimi #12).
export { extractConfigPayload } from './envelope';

const DEMO_CONFIG_KEY = 'rbgrid.demo.config';
/** Demo restore point (in-memory, one slot like the hub). */
let demoBackup: { ts: number; fw: string; files: number } | null = null;
/** Demo access point (in-memory): the first-login password until the visitor sets one. */
const demoAp: ApStatus = { ssid: 'rbgrid-7e01', userSet: false };
/** Demo CORS policy (in-memory, like the hub's default: open). */
let demoSecurity: SecurityConfig = { dev_cors: false, origins: [], builtin_origins: ['https://webapp.rbgrid.io'], origins_max: 8 };

function demoOverrides(): Record<string, JsonObject> {
  try { return JSON.parse(localStorage.getItem(DEMO_CONFIG_KEY) ?? '{}') as Record<string, JsonObject>; }
  catch { return {}; }
}

/** Demo config = canned mock + whatever was written in this browser. */
function demoReadConfig(domain: ConfigDomain): JsonObject {
  const doc = { ...mockConfig(domain), ...(demoOverrides()[domain] ?? {}) };
  return domain === 'units' ? (demoAdjustUnitsConfig(doc) as JsonObject) : doc;
}

function demoWriteConfig(domain: ConfigDomain, doc: JsonObject): void {
  try {
    const all = demoOverrides();
    all[domain] = { ...(all[domain] ?? {}), ...doc };
    localStorage.setItem(DEMO_CONFIG_KEY, JSON.stringify(all));
  } catch { /* storage unavailable → demo stays read-only */ }
}

export class DirectTransport implements DeviceTransport {
  readonly kind: TransportKind = 'direct';

  private get api() {
    return getApiClient();
  }

  async getSchema(): Promise<RawSchema | null> {
    if (isDemo()) return mockSchema(); // demo shows the full feature set
    const base = getBaseUrl();
    if (!base) return null;
    try {
      const res = await axios.get<RawSchema | { data: RawSchema }>(`${base}/api/v1/schema`, { timeout: 5000 });
      const body = res.data as RawSchema & { data?: RawSchema };
      // tolerate either a bare schema or an enveloped { data: schema }
      return (body.data ?? body) as RawSchema;
    } catch {
      return null; // 404 / unreachable / older fw → caller falls back to baseline caps
    }
  }

  async getConfig(domain: ConfigDomain): Promise<JsonObject> {
    if (isDemo()) return demoReadConfig(domain);
    const path = CONFIG_DOMAIN_PATH[domain];
    if (!path) throw new UnsupportedDomainError(domain, this.kind);
    const base = getBaseUrl();
    if (!base) return {};
    // Device config GETs come in THREE envelope shapes (verified on hw 2026-06-29):
    //   {type,…,data:{…}} (time/hardware/tariff/units/telemetry/wifi) · {<section>:{…}} (system) ·
    //   bare {…} (mqtt). extractConfigPayload normalizes all three to the inner config object;
    //   the generic ShadowConfigEditor then renders whatever fields are there (no static shape).
    const res = await axios.get(`${base}${path}`, { headers: tokenStore.authHeader(), timeout: 5000 });
    return extractConfigPayload(res.data);
  }

  async setConfig(domain: ConfigDomain, doc: JsonObject): Promise<SetConfigResult | void> {
    // Demo behaves like a device: the write is kept (this browser only) and read back, so
    // e.g. the scenario chosen in Настройки really switches the Overview in demo mode.
    if (isDemo()) { demoWriteConfig(domain, doc); return; }
    const path = CONFIG_DOMAIN_PATH[domain];
    const body = buildConfigPostBody(domain, doc);
    if (!path || body === null) throw new UnsupportedDomainError(domain, this.kind);
    // Through the api client: Bearer (also set explicitly — B-14 ordering), typed errors, no retry.
    const res = (await this.api.postConfig(path, body, { headers: tokenStore.authHeader() })) as
      { requires_reboot?: unknown; data?: { requires_reboot?: unknown } } | null;
    const flag = res?.requires_reboot ?? res?.data?.requires_reboot;
    return typeof flag === 'boolean' ? { requiresReboot: flag } : {};
  }

  async setMqttBroker(uri: string): Promise<SetConfigResult> {
    if (isDemo()) {
      // Like the hub: stored now, the client keeps its broker until a restart.
      demoWriteConfig('mqtt', { broker_uri: uri, persisted: true });
      return { requiresReboot: true };
    }
    const res = (await this.api.postConfig('/api/system/mqtt-config', { broker_uri: uri }, { headers: tokenStore.authHeader() })) as
      { requires_reboot?: unknown } | null;
    return { requiresReboot: res?.requires_reboot !== false };
  }

  async patchUnit(hexId: string, patch: JsonObject): Promise<JsonObject> {
    if (isDemo()) {
      const cur = demoReadConfig('units');
      const list = Array.isArray(cur.units) ? (cur.units as JsonObject[]) : [];
      const id = parseInt(hexId, 16);
      const units = list.map((u) => (u.unit_id === id ? { ...u, ...patch } : u));
      demoWriteConfig('units', { units });
      return units.find((u) => u.unit_id === id) ?? {};
    }
    // Small body (hub caps it at 512 B); same typed errors as config writes, no retry.
    return (await this.api.postConfig(`/api/v1/units/${hexId}`, patch, { headers: tokenStore.authHeader() })) as JsonObject;
  }

  async getProvisioning(): Promise<ProvisioningStatus | null> {
    if (isDemo()) return mockProvisioning();
    const base = getBaseUrl();
    if (!base) return null;
    try {
      const res = await axios.get<ProvisioningStatus | { data: ProvisioningStatus }>(
        `${base}/api/system/provisioning`,
        { headers: tokenStore.authHeader(), timeout: 5000 },
      );
      const body = res.data as ProvisioningStatus & { data?: ProvisioningStatus };
      return (body.data ?? body) as ProvisioningStatus;
    } catch {
      return null;
    }
  }

  async getEconomics(unit: string): Promise<Economics | null> {
    if (isDemo()) return mockEconomics(unit);
    return this.getBare<Economics>('/api/v1/economics', { unit });
  }

  async getSummary(unit: string, period: SummaryPeriod): Promise<Summary | null> {
    if (isDemo()) return mockSummary(unit, period);
    return this.getBare<Summary>('/api/v1/summary', { unit, period });
  }

  async setTelemetryProfile(profile: 'lean' | 'full'): Promise<void> {
    if (isDemo()) return;
    const base = getBaseUrl();
    if (!base) return;
    await axios.post(`${base}/api/system/telemetry-config`, { profile }, {
      headers: tokenStore.authHeader(), timeout: 5000,
    });
  }

  async getWsTicket(): Promise<string> {
    const res = await axios.post<{ ticket?: string; data?: { ticket?: string } }>(`${getBaseUrl()}/api/v1/ws-ticket`, undefined, {
      headers: tokenStore.authHeader(), timeout: 5000,
    });
    const ticket = res.data?.ticket ?? res.data?.data?.ticket;
    if (!ticket) throw new Error('no ticket');
    return ticket;
  }

  async getTokenStatus(): Promise<boolean> {
    if (isDemo()) return tokenStore.has();
    const base = getBaseUrl();
    if (!base) return false;
    try {
      const res = await axios.get<{ token_set?: boolean } | { data: { token_set: boolean } }>(
        `${base}/api/system/token`, { timeout: 5000 },
      );
      const body = res.data as { token_set?: boolean; data?: { token_set?: boolean } };
      return Boolean((body.data ?? body).token_set);
    } catch {
      return false;
    }
  }

  async getTokenRoles(): Promise<TokenRoles> {
    if (isDemo()) return { adminSet: tokenStore.has(), readSet: false };
    const base = getBaseUrl();
    if (!base) return { adminSet: false, readSet: false };
    try {
      const res = await axios.get<Record<string, unknown>>(`${base}/api/system/token`, { timeout: 5000 });
      const body = (res.data.data ?? res.data) as { token_set?: boolean; read_token_set?: boolean };
      return { adminSet: Boolean(body.token_set), readSet: Boolean(body.read_token_set) };
    } catch {
      return { adminSet: false, readSet: false };
    }
  }

  async setToken(token: string, role: TokenRole = 'admin'): Promise<void> {
    if (!isDemo()) {
      const base = getBaseUrl();
      if (!base) return;
      // rotate needs the CURRENT token as Bearer; initial set is open (fail-open window).
      await axios.post(`${base}/api/system/token`, { token, role }, {
        headers: tokenStore.authHeader(), timeout: 5000,
      });
    }
    // Mirror the ADMIN token locally so subsequent auth-guarded writes carry it (or none).
    // A read-role token is the device's, not our write credential — don't overwrite the store.
    if (role === 'admin') {
      if (token) tokenStore.set(token); else tokenStore.clear();
    }
  }

  async getAp(): Promise<ApStatus | null> {
    if (isDemo()) return { ...demoAp };
    const base = getBaseUrl();
    if (!base) return null;
    // /ap needs the ADMIN token: with a read token it is 403 on every load (root ws-ticket-app-recheck).
    // Once refused, this tab doesn't ask again with the same token (a fingerprint, never the token).
    const key = `rbgrid.apForbidden@${base}`;
    const fp = tokenFingerprint(tokenStore.get());
    try { if (sessionStorage.getItem(key) === fp) return null; } catch { /* storage blocked */ }
    try {
      const res = await axios.get<Record<string, unknown>>(`${base}/api/system/ap`, { headers: tokenStore.authHeader(), timeout: 5000 });
      const body = (res.data.data ?? res.data) as { ssid?: string; user_set?: boolean; per_device?: boolean };
      // `per_device` = the older name of the same flag (root 20.10).
      return { ssid: String(body.ssid ?? ''), userSet: Boolean(body.user_set ?? body.per_device) };
    } catch (e) {
      if ((e as { response?: { status?: number } })?.response?.status === 403) {
        try { sessionStorage.setItem(key, fp); } catch { /* storage blocked */ }
      }
      return null;
    }
  }

  async setApPassword(password: string): Promise<{ applied?: boolean }> {
    if (isDemo()) { demoAp.userSet = password !== ''; return { applied: true }; }
    const base = getBaseUrl();
    if (!base) return {};
    const res = await axios.post<Record<string, unknown>>(`${base}/api/system/ap`, { password }, { headers: tokenStore.authHeader(), timeout: 8000 });
    const body = (res.data?.data ?? res.data ?? {}) as { applied?: boolean };
    return typeof body.applied === 'boolean' ? { applied: body.applied } : {};
  }

  async getSecurity(): Promise<SecurityConfig | null> {
    if (isDemo()) return { ...demoSecurity, origins: [...demoSecurity.origins] };
    const base = getBaseUrl();
    if (!base) return null;
    try {
      const res = await axios.get<Record<string, unknown>>(`${base}/api/system/security`, {
        headers: tokenStore.authHeader(), timeout: 5000,
      });
      const body = (res.data.data ?? res.data) as { dev_cors?: boolean; origins?: string[]; builtin_origins?: string[]; origins_max?: number };
      return {
        dev_cors: Boolean(body.dev_cors), origins: Array.isArray(body.origins) ? body.origins : [],
        ...(Array.isArray(body.builtin_origins) ? { builtin_origins: body.builtin_origins } : {}),
        ...(typeof body.origins_max === 'number' ? { origins_max: body.origins_max } : {}),
      };
    } catch {
      return null;
    }
  }

  async setSecurity(cfg: SecurityConfig): Promise<void> {
    if (isDemo()) { demoSecurity = { ...demoSecurity, dev_cors: cfg.dev_cors, origins: [...cfg.origins] }; return; }
    const base = getBaseUrl();
    if (!base) return;
    // Always both keys, dev_cors explicit (v1.1.3: a POST without it means allow-list); never the built-ins.
    await axios.post(`${base}/api/system/security`, { dev_cors: cfg.dev_cors === true, origins: cfg.origins }, {
      headers: tokenStore.authHeader(), timeout: 5000,
    });
  }

  async getBackupStatus(): Promise<BackupStatus | null> {
    if (isDemo()) return demoBackup ? { exists: true, files: 6, meta: demoBackup } : { exists: false, files: 0 };
    const base = getBaseUrl();
    if (!base) return null;
    try {
      const res = await axios.get<Record<string, unknown>>(`${base}/api/system/backup`, {
        headers: tokenStore.authHeader(), timeout: 5000,
      });
      const b = (res.data.data ?? res.data) as { exists?: boolean; files?: number; sealed?: unknown; meta?: BackupStatus['meta'] };
      return { exists: Boolean(b.exists), files: Number(b.files ?? 0), meta: b.meta, ...(typeof b.sealed === 'boolean' ? { sealed: b.sealed } : {}) };
    } catch {
      return null;
    }
  }

  async createBackup(): Promise<{ filesBackedUp: number }> {
    if (isDemo()) { demoBackup = { ts: Math.floor(Date.now() / 1000), fw: 'demo', files: 6 }; return { filesBackedUp: 6 }; }
    const base = getBaseUrl();
    if (!base) return { filesBackedUp: 0 };
    // Empty body: the device snapshots the current configs into the slot.
    const res = await axios.post<Record<string, unknown>>(`${base}/api/system/backup`, undefined, {
      headers: tokenStore.authHeader(), timeout: 10000,
    });
    const b = (res.data.data ?? res.data) as { files_backed_up?: number };
    return { filesBackedUp: Number(b.files_backed_up ?? 0) };
  }

  async restoreBackup(): Promise<RestoreResult> {
    if (isDemo()) return demoBackup ? { status: 'OK', filesRestored: 6, requiresReboot: true } : { status: 'ERROR', filesRestored: 0, requiresReboot: false };
    const base = getBaseUrl();
    if (!base) return { status: 'ERROR', filesRestored: 0, requiresReboot: false };
    // Empty body: restore from the on-device slot. 404 ⇒ no backup to restore.
    const res = await axios.post<Record<string, unknown>>(`${base}/api/system/restore`, undefined, {
      headers: tokenStore.authHeader(), timeout: 15000,
    });
    const b = (res.data.data ?? res.data) as { status?: string; files_restored?: number; requires_reboot?: boolean };
    return {
      status: String(b.status ?? 'OK'),
      filesRestored: Number(b.files_restored ?? 0),
      requiresReboot: Boolean(b.requires_reboot),
    };
  }

  async postOta(image: ArrayBuffer | Blob, opts?: OtaOptions): Promise<OtaResult> {
    if (isDemo()) return { status: 'ok', message: 'flashed, rebooting', partition: 'ota_0' };
    const base = getBaseUrl();
    if (!base) throw new Error('OTA: no device base URL');
    const headers: Record<string, string> = {
      ...tokenStore.authHeader(),
      'Content-Type': 'application/octet-stream',
    };
    const fw: Record<string, string> = {};
    if (opts?.sha256) fw['X-FW-SHA256'] = opts.sha256;
    if (opts?.version) fw['X-FW-Version'] = opts.version;
    // No in-band progress; surface upload progress via XHR events. ≥120s: ~1.5MB + validate.
    const send = (extra: Record<string, string>) => axios.post<Record<string, unknown>>(`${base}/api/system/ota`, image, {
      headers: { ...headers, ...extra },
      timeout: 120000,
      onUploadProgress: opts?.onProgress
        ? (e) => { if (e.total) opts.onProgress!(e.loaded / e.total); }
        : undefined,
    });
    let res;
    try {
      res = await send(fw);
    } catch (err) {
      // Hubs before 0d67ae6f don't list X-FW-* in their CORS preflight: cross-origin the browser
      // drops the upload unsent — send it once more without them.
      const e = err as { response?: unknown; code?: string };
      if (!Object.keys(fw).length || deviceSameOrigin() || e.response || e.code === 'ECONNABORTED' || e.code === 'ETIMEDOUT') throw err;
      res = await send({});
    }
    // ⚠ The device self-reboots ~1s after this 200 → caller should poll GET /version.
    const b = (res.data.data ?? res.data) as { status?: string; message?: string; partition?: string };
    return { status: String(b.status ?? 'ok'), message: String(b.message ?? ''), partition: String(b.partition ?? '') };
  }

  async getEspnow(): Promise<EspnowStatus | null> {
    if (isDemo()) return mockEspnow();
    return this.getBare<EspnowStatus>('/api/v1/espnow', {});
  }

  async espnowAction(action: EspnowAction, params: EspnowActionParams = {}): Promise<EspnowActionResult> {
    if (isDemo()) return { ok: true, reboot_to_register: action === 'add' };
    const base = getBaseUrl();
    if (!base) return { ok: false, message: 'not connected' };
    try {
      const res = await axios.post<EspnowActionResult>(
        `${base}/api/v1/espnow`, { action, ...params },
        { headers: tokenStore.authHeader(), timeout: 5000 },
      );
      return res.data;
    } catch (err) {
      // fw 1.1.7 refuses with {ok:false, error, message} on 400/409/503 — that's an answer, not a lost request.
      const data = (err as { response?: { data?: unknown } }).response?.data as EspnowActionResult | undefined;
      if (data && typeof data === 'object' && data.ok === false) return data;
      throw err;
    }
  }

  async exportCsv(params: { unit: string; period: string; type: string; from?: number; to?: number }): Promise<Blob | null> {
    if (isDemo()) {
      return new Blob([demoExportCsv(params.type, params.period)], { type: 'text/csv' });
    }
    const base = getBaseUrl();
    if (!base) return null;
    try {
      const res = await axios.get(`${base}/api/v1/export.csv`, {
        params, responseType: 'blob', headers: tokenStore.authHeader(), timeout: 15000,
      });
      return res.data as Blob;
    } catch {
      return null;
    }
  }

  /** GET a BARE v1 endpoint (tolerates an enveloped { data } too). */
  private async getBare<T>(path: string, params: Record<string, string>): Promise<T | null> {
    const base = getBaseUrl();
    if (!base) return null;
    try {
      // economics/summary/espnow are gated data reads once a READ token is armed (rbapi_require_read):
      // without the Bearer they 401 and read as «no data» (Kimi #2).
      const res = await axios.get<T | { data: T }>(`${base}${path}`, { params, headers: tokenStore.authHeader(), timeout: 5000 });
      const body = res.data as T & { data?: T };
      return (body.data ?? body) as T;
    } catch {
      return null;
    }
  }

}

/** A short non-reversible fingerprint of the token (FNV-1a) — plain http has no crypto.subtle. */
function tokenFingerprint(token: string | null): string {
  let h = 0x811c9dc5;
  for (const ch of token ?? '') { h ^= ch.charCodeAt(0); h = Math.imul(h, 0x01000193) >>> 0; }
  return h.toString(16);
}

/** Thrown when a telemetry doc has no writable `profile` (the only POSTable telemetry field). */
export class InvalidConfigDocError extends Error {
  constructor(public readonly domain: ConfigDomain, message: string) {
    super(message);
    this.name = 'InvalidConfigDocError';
  }
}

/**
 * Re-wrap the shadow's INNER config doc into the exact body the device's POST handler parses
 * (see the table at the top of this file). null = the domain has no writable REST route.
 */
export function buildConfigPostBody(domain: ConfigDomain, doc: JsonObject): JsonObject | null {
  switch (domain) {
    case 'system':
      return { system: doc };
    case 'units':
      return { type: 'units_config', data: doc };
    case 'tariff':
      return { type: 'tariff_plan_list_config', data: doc };
    case 'hardware':
      // Canon 2beb4bc8 accepts the GET envelope verbatim (and legacy "hardware"); the
      // device's requires_reboot is genuine here → the shadow's reboot path applies.
      return { type: 'hardware_config', data: doc };
    case 'telemetry': {
      const profile = doc.profile;
      if (profile !== 'lean' && profile !== 'full') {
        throw new InvalidConfigDocError(domain, 'telemetry-config POST accepts only profile = "lean" | "full"');
      }
      return { profile };
    }
    default:
      return null; // time / wifi (GET-only), mqtt / espnow / security
  }
}
