import axios, { AxiosRequestConfig, AxiosResponse } from 'axios';
import { DirectTransport } from './DirectTransport';
import { connectDevice } from '../../services/connection';
import { tokenStore } from '../auth/authWiring';

/** Data reads outside the shared api client still carry the Bearer: a hub with a READ token armed
 *  401s economics/summary/espnow without it, and the app read that as «no data» (Kimi #2). */
let seen: Array<{ url: string; auth: unknown }> = [];

beforeAll(() => {
  connectDevice('192.168.1.139');
  axios.defaults.adapter = async (config: AxiosRequestConfig): Promise<AxiosResponse> => {
    seen.push({ url: String(config.url), auth: (config.headers as Record<string, unknown> | undefined)?.Authorization });
    return { data: { data: {} }, status: 200, statusText: 'OK', headers: {}, config } as AxiosResponse;
  };
  tokenStore.set('t0ken');
});
afterAll(() => tokenStore.clear());
beforeEach(() => { seen = []; });

describe('DirectTransport — gated GETs send the token', () => {
  const t = new DirectTransport();
  it.each([
    ['economics', () => t.getEconomics('input')],
    ['summary', () => t.getSummary('input', 'month' as never)],
    ['espnow', () => t.getEspnow()],
    ['provisioning', () => t.getProvisioning()],
  ])('%s', async (_name, call) => {
    await call();
    expect(seen).toHaveLength(1);
    expect(seen[0].auth).toBe('Bearer t0ken');
  });
});

describe('GET /api/system/ap with a read token (root ws-ticket-app-recheck)', () => {
  it('a 403 is not asked again in this tab with the same token; a new token asks again', async () => {
    const calls: string[] = [];
    axios.defaults.adapter = async (config: AxiosRequestConfig): Promise<AxiosResponse> => {
      calls.push(String(config.url));
      const res = { data: {}, status: 403, statusText: 'Forbidden', headers: {}, config } as AxiosResponse;
      throw Object.assign(new Error('403'), { response: res });
    };
    const t = new DirectTransport();
    tokenStore.set('read-token');
    expect(await t.getAp()).toBeNull();
    expect(await t.getAp()).toBeNull();
    expect(calls).toHaveLength(1);
    expect(JSON.stringify(sessionStorage)).not.toContain('read-token');
    tokenStore.set('admin-token');
    await t.getAp();
    expect(calls).toHaveLength(2);
  });
});

describe('GET /api/system/backup — sealed (fw 1.1.7, root backup-sealed-flag)', () => {
  const t = new DirectTransport();
  const answer = (body: unknown) => {
    axios.defaults.adapter = async (config: AxiosRequestConfig): Promise<AxiosResponse> =>
      ({ data: body, status: 200, statusText: 'OK', headers: {}, config } as AxiosResponse);
  };
  it('true / false pass through; absent stays absent', async () => {
    answer({ exists: true, files: 6, sealed: false });
    expect((await t.getBackupStatus())?.sealed).toBe(false);
    answer({ data: { exists: true, files: 6, sealed: true } });
    expect((await t.getBackupStatus())?.sealed).toBe(true);
    answer({ exists: true, files: 6 });
    expect(await t.getBackupStatus()).not.toHaveProperty('sealed');
  });
});

describe('POST /api/v1/espnow refusals (fw 1.1.7)', () => {
  it('a 409 {ok:false, error} comes back as the answer; a bare 500 still throws', async () => {
    const t = new DirectTransport();
    axios.defaults.adapter = async (config: AxiosRequestConfig): Promise<AxiosResponse> => {
      const res = { data: { ok: false, error: 'ESPNOW_DISABLED', message: 'ESP-NOW is off' }, status: 409, statusText: 'Conflict', headers: {}, config } as AxiosResponse;
      throw Object.assign(new Error('409'), { response: res, config, isAxiosError: true });
    };
    expect(await t.espnowAction('pair', { seconds: 60 })).toMatchObject({ ok: false, error: 'ESPNOW_DISABLED' });
    axios.defaults.adapter = async (config: AxiosRequestConfig): Promise<AxiosResponse> => {
      throw Object.assign(new Error('500'), { response: { data: 'oops', status: 500, config }, config, isAxiosError: true });
    };
    await expect(t.espnowAction('latch')).rejects.toThrow();
  });
});
