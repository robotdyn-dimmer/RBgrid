/**
 * MqttTransport — the broker backend (single device, remote) over the device's MQTT
 * command channel. Forms confirmed FINAL by root 2026-06-23: cmd-get data is byte-identical
 * to the HTTP endpoints (+ top-level id echo for correlation), so the same types/parsers
 * apply with zero divergence.
 *
 * Supported over cmd: schema, config get (system/telemetry/wifi/tariff/units), config set
 * (tariff/units, full-envelope round-trip), economics, summary, espnow (read). Ops that
 * are not cmd sections (token, telemetry-profile set, espnow actions, CSV export) stay on
 * the Direct transport.
 *
 * Takes a CmdChannel (injectable → unit-testable with a fake broker). The concrete
 * mqtt.js-over-wss client (BrowserMqttClient) is wired by the TransportRouter once the
 * broker's wss listener exists (infra). Until then this compiles + is tested but not run.
 */
import { JsonObject } from '../json';
import { ConfigDomain } from '../shadow/ConfigShadow';
import { RawSchema } from '../schema/capabilities';
import { ProvisioningStatus } from '../provisioning/ProvisioningFSM';
import { Economics, Summary, SummaryPeriod } from '../economics/types';
import { EspnowStatus, EspnowAction, EspnowActionParams, EspnowActionResult } from '../espnow/types';
import {
  DeviceTransport, TransportKind, NotImplementedError, UnsupportedDomainError,
  TokenRole, TokenRoles, SecurityConfig, ApStatus, BackupStatus, RestoreResult, OtaResult, OtaOptions,
} from './types';
import { CmdChannel } from './mqtt/CmdChannel';

/** ConfigDomain → MQTT cmd section (only the sections the device exposes over cmd). */
const GET_SECTION: Partial<Record<ConfigDomain, string>> = {
  system: 'system',
  telemetry: 'telemetry',
  wifi: 'wifi',
  tariff: 'tariff',
  units: 'units',
};
const SET_SECTION: Partial<Record<ConfigDomain, string>> = {
  tariff: 'tariff',
  units: 'units',
};

interface Envelope { data?: unknown; [k: string]: unknown; }

export class MqttTransport implements DeviceTransport {
  readonly kind: TransportKind = 'mqtt';

  constructor(private readonly cmd: CmdChannel) {}

  private async getSection<T>(section: string, extra: Record<string, unknown> = {}): Promise<T | null> {
    const resp = await this.cmd.request('get', { section, ...extra });
    const env = resp as Envelope;
    return (env.data ?? resp) as T;
  }

  async getSchema(): Promise<RawSchema | null> {
    return this.getSection<RawSchema>('schema');
  }

  async getConfig(domain: ConfigDomain): Promise<JsonObject> {
    const section = GET_SECTION[domain];
    if (!section) throw new UnsupportedDomainError(domain, this.kind);
    const resp = await this.cmd.get(section);
    return ((resp as Envelope).data ?? {}) as JsonObject;
  }

  async setConfig(domain: ConfigDomain, doc: JsonObject): Promise<void> {
    const section = SET_SECTION[domain];
    if (!section) throw new UnsupportedDomainError(domain, this.kind);
    // SET round-trip gotcha: data must be the FULL envelope a get returned, with the
    // inner `data` replaced by the new doc.
    const current = await this.cmd.get(section);
    const envelope = { ...(current as Envelope), data: doc };
    await this.cmd.set(section, envelope);
  }

  async getProvisioning(): Promise<ProvisioningStatus | null> {
    return null; // not a cmd section — provisioning is read over the Direct transport
  }

  async getEconomics(unit: string): Promise<Economics | null> {
    return this.getSection<Economics>('economics', { unit });
  }

  async getSummary(unit: string, period: SummaryPeriod): Promise<Summary | null> {
    return this.getSection<Summary>('summary', { unit, period });
  }

  async getEspnow(): Promise<EspnowStatus | null> {
    return this.getSection<EspnowStatus>('espnow');
  }

  // --- not exposed over the cmd channel → Direct-only ---
  async setTelemetryProfile(_profile: 'lean' | 'full'): Promise<void> {
    throw new NotImplementedError('setTelemetryProfile', this.kind);
  }
  async getTokenStatus(): Promise<boolean> {
    throw new NotImplementedError('getTokenStatus', this.kind);
  }
  async getTokenRoles(): Promise<TokenRoles> {
    throw new NotImplementedError('getTokenRoles', this.kind);
  }
  async setToken(_token: string, _role?: TokenRole): Promise<void> {
    throw new NotImplementedError('setToken', this.kind);
  }
  async getAp(): Promise<ApStatus | null> {
    return null;
  }
  async setApPassword(_password: string): Promise<{ applied?: boolean }> {
    throw new NotImplementedError('setApPassword', this.kind);
  }
  async getSecurity(): Promise<SecurityConfig | null> {
    throw new NotImplementedError('getSecurity', this.kind);
  }
  async setSecurity(_cfg: SecurityConfig): Promise<void> {
    throw new NotImplementedError('setSecurity', this.kind);
  }
  async getBackupStatus(): Promise<BackupStatus | null> {
    throw new NotImplementedError('getBackupStatus', this.kind);
  }
  async createBackup(): Promise<{ filesBackedUp: number }> {
    throw new NotImplementedError('createBackup', this.kind);
  }
  async restoreBackup(): Promise<RestoreResult> {
    throw new NotImplementedError('restoreBackup', this.kind);
  }
  async postOta(_image: ArrayBuffer | Blob, _opts?: OtaOptions): Promise<OtaResult> {
    throw new NotImplementedError('postOta', this.kind);
  }
  async espnowAction(_action: EspnowAction, _params?: EspnowActionParams): Promise<EspnowActionResult> {
    throw new NotImplementedError('espnowAction (POST not over cmd)', this.kind);
  }
  async exportCsv(): Promise<Blob | null> {
    throw new NotImplementedError('exportCsv (HTTP-only)', this.kind);
  }
}
