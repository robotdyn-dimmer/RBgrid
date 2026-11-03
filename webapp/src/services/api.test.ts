import { AxiosError, AxiosRequestConfig, AxiosResponse } from 'axios';
import {
  RBGridApiClient, ApiRequestError, NotInBuildError, AdminRequiredError, AuthRequiredError,
  StorageMigratingError,
} from './api';
import { connectDevice } from './connection';

/** A scripted reply: status + body (+ headers), or 'network' for a transport failure. */
type Reply = { status: number; data?: unknown; headers?: Record<string, string> } | 'network';

interface Seen { method: string; url: string; data: unknown }

/** Build a client whose axios adapter replays `replies` in order and records every request. */
function clientWith(replies: Reply[]) {
  const seen: Seen[] = [];
  const client = new RBGridApiClient({
    baseURL: '',
    retryConfig: { MAX_ATTEMPTS: 3, INITIAL_DELAY: 1000, MAX_DELAY: 5000, BACKOFF_FACTOR: 2 } as never,
  });
  const queue = [...replies];
  client.getAxiosInstance().defaults.adapter = async (config: AxiosRequestConfig): Promise<AxiosResponse> => {
    seen.push({
      method: String(config.method).toUpperCase(),
      url: String(config.url),
      data: typeof config.data === 'string' ? JSON.parse(config.data) : config.data,
    });
    const r = queue.shift() ?? { status: 200, data: {} };
    if (r === 'network') {
      throw new AxiosError('Network Error', 'ERR_NETWORK', config as never);
    }
    const response = {
      data: r.data, status: r.status, statusText: String(r.status), headers: r.headers ?? {}, config,
    } as AxiosResponse;
    if (r.status >= 400) {
      throw new AxiosError(`Request failed with status code ${r.status}`, 'ERR_BAD_RESPONSE', config as never, null, response);
    }
    return response;
  };
  return { client, seen };
}

/** Make the interceptor's backoff sleeps instant, recording the requested delays. */
function instantTimers(): number[] {
  const delays: number[] = [];
  jest.spyOn(global, 'setTimeout').mockImplementation(((fn: () => void, ms?: number) => {
    delays.push(ms ?? 0);
    fn();
    return 0 as unknown as NodeJS.Timeout;
  }) as never);
  return delays;
}

beforeAll(() => connectDevice('192.168.1.139'));
afterEach(() => jest.restoreAllMocks());

describe('interceptor retry policy — B-3', () => {
  it('GET is retried on 5xx/network with exponential backoff, then succeeds', async () => {
    const delays = instantTimers();
    const { client, seen } = clientWith(['network', { status: 500 }, { status: 200, data: { ok: 1 } }]);
    const res = await client.getAxiosInstance().get('/api/x');
    expect(res.data).toEqual({ ok: 1 });
    expect(seen).toHaveLength(3);
    expect(delays).toEqual([1000, 2000]);
  });

  it('GET gives up after MAX_ATTEMPTS retries and keeps the status', async () => {
    instantTimers();
    const { client, seen } = clientWith([{ status: 502 }, { status: 502 }, { status: 502 }, { status: 502 }]);
    const err = await client.getAxiosInstance().get('/api/x').catch((e) => e);
    expect(seen).toHaveLength(4);
    expect(err).toBeInstanceOf(ApiRequestError);
    expect(err).toBeInstanceOf(Error);
    expect(err.status).toBe(502);
  });

  it.each(['post', 'put', 'delete'] as const)('%s is NEVER retried (5xx and network)', async (m) => {
    instantTimers();
    for (const reply of [{ status: 500 }, 'network'] as Reply[]) {
      const { client, seen } = clientWith([reply, { status: 200 }]);
      const ax = client.getAxiosInstance();
      const call = m === 'delete' ? ax.delete('/api/v1/units/0007') : ax[m]('/api/system/system-config', { a: 1 });
      await expect(call).rejects.toBeInstanceOf(ApiRequestError);
      expect(seen).toHaveLength(1);
    }
  });

  it('4xx is not retried; status/headers/data are preserved', async () => {
    instantTimers();
    const { client, seen } = clientWith([
      { status: 400, data: { status: 'ERROR', message: 'Missing system', error_code: 'MISSING_SYSTEM_SECTION' }, headers: { 'X-Req': '7' } },
    ]);
    const err = await client.getAxiosInstance().get('/api/x').catch((e) => e);
    expect(seen).toHaveLength(1);
    expect(err.status).toBe(400);
    expect(err.headers['x-req']).toBe('7');
    expect(err.data).toEqual({ status: 'ERROR', message: 'Missing system', error_code: 'MISSING_SYSTEM_SECTION' });
    expect(err.code).toBe('MISSING_SYSTEM_SECTION');
    expect(err.message).toBe('Missing system');
    expect(err.response.status).toBe(400); // AxiosError-compatible view
  });

  it('501 → NotInBuildError, not retried', async () => {
    instantTimers();
    const { client, seen } = clientWith([{ status: 501, data: { error: 'not_in_build', build: 'oss' } }]);
    const err = await client.getAxiosInstance().get('/api/v1/economics').catch((e) => e);
    expect(seen).toHaveLength(1);
    expect(err).toBeInstanceOf(NotInBuildError);
    expect(err.httpStatus).toBe(501);
  });

  it('401 → AuthRequiredError, 403 → AdminRequiredError', async () => {
    const a = clientWith([{ status: 401, data: { error: 'unauthorized' } }]);
    await expect(a.client.getAxiosInstance().post('/api/system/system-config', {})).rejects.toBeInstanceOf(AuthRequiredError);
    const b = clientWith([{ status: 403, data: { error: 'admin_required' } }]);
    await expect(b.client.getAxiosInstance().post('/api/system/system-config', {})).rejects.toBeInstanceOf(AdminRequiredError);
  });

  it('503 + Retry-After is honoured (waits exactly Retry-After)', async () => {
    const delays = instantTimers();
    const { client, seen } = clientWith([
      { status: 503, data: { status: 'initializing' }, headers: { 'Retry-After': '5' } },
      { status: 200, data: { sensors: [] } },
    ]);
    await client.getAxiosInstance().get('/api/v1/sensors');
    expect(seen).toHaveLength(2);
    expect(delays).toEqual([5000]);
  });

  it('503 with a long Retry-After is surfaced immediately with retryAfterSec', async () => {
    instantTimers();
    const { client, seen } = clientWith([{ status: 503, data: {}, headers: { 'Retry-After': '60' } }]);
    const err = await client.getAxiosInstance().get('/api/x').catch((e) => e);
    expect(seen).toHaveLength(1);
    expect(err.status).toBe(503);
    expect(err.retryAfterSec).toBe(60);
  });

  it('503 storage migration → StorageMigratingError (no retry, progress + Retry-After)', async () => {
    instantTimers();
    const { client, seen } = clientWith([
      { status: 503, data: { error: 'not_ready', storage_state: 'migrating', progress: 42 }, headers: { 'Retry-After': '30' } },
    ]);
    const err = await client.getHistorySince({ unit_id: '0007', period: 'hour', type: 'energy', since: 0 } as never)
      .catch((e) => e);
    expect(seen).toHaveLength(1);
    expect(err).toBeInstanceOf(StorageMigratingError);
    expect(err.progress).toBe(42);
    expect(err.retryAfterSec).toBe(30);
  });
});

describe('createUnit / deleteUnit structured 4xx — B-3', () => {
  it('createUnit returns the structured 409 body inline (single POST)', async () => {
    const { client, seen } = clientWith([{ status: 409, data: { status: 'error', message: 'already bound' } }]);
    const r = await client.createUnit({ iface: 'i2c', addr: 1 } as never);
    expect(r).toEqual({ status: 'error', message: 'already bound' });
    expect(seen).toHaveLength(1);
  });

  it('deleteUnit returns the structured 404 body inline', async () => {
    const { client } = clientWith([{ status: 404, data: { status: 'error', message: 'not found' } }]);
    await expect(client.deleteUnit('0007')).resolves.toEqual({ status: 'error', message: 'not found' });
  });

  it('createUnit still throws on 5xx (no retry) and on 403', async () => {
    const a = clientWith([{ status: 507, data: { status: 'error', message: 'full' } }]);
    await expect(a.client.createUnit({} as never)).rejects.toBeInstanceOf(ApiRequestError);
    expect(a.seen).toHaveLength(1);
    const b = clientWith([{ status: 403, data: { status: 'error', message: 'admin' } }]);
    await expect(b.client.createUnit({} as never)).rejects.toBeInstanceOf(AdminRequiredError);
  });
});

describe('legacy update* methods post the device envelopes — B-2', () => {
  it('updateSystemConfig wraps the inner doc as {system:{…}} (no legacy validator)', async () => {
    const { client, seen } = clientWith([{ status: 200, data: { status: 'OK', requires_reboot: true } }]);
    await client.updateSystemConfig({ site_profile: 'industrial' } as never);
    expect(seen[0]).toMatchObject({ method: 'POST', url: '/api/system/system-config', data: { system: { site_profile: 'industrial' } } });
  });

  it('updateSystemConfig passes an existing envelope through unchanged', async () => {
    const { client, seen } = clientWith([{ status: 200, data: {} }]);
    await client.updateSystemConfig({ system: { site_profile: 'home' } } as never);
    expect(seen[0].data).toEqual({ system: { site_profile: 'home' } });
  });

  it('time / wifi have no POST route → reject before the network', async () => {
    const { client, seen } = clientWith([]);
    await expect(client.updateTimeConfig({} as never)).rejects.toThrow(/read-only/);
    await expect(client.updateWiFiConfig({} as never)).rejects.toThrow(/read-only/);
    expect(seen).toHaveLength(0);
  });
});

describe('startModuleFirmware — hub without X-FW-* in its CORS preflight', () => {
  it('cross-origin network failure with the hash → sent once more without it', async () => {
    const { client, seen } = clientWith(['network', { status: 202, data: { status: 'started' } }]);
    const sent: (string | undefined)[] = [];
    const adapter = client.getAxiosInstance().defaults.adapter as (c: AxiosRequestConfig) => Promise<AxiosResponse>;
    client.getAxiosInstance().defaults.adapter = (c: AxiosRequestConfig) => {
      sent.push((c.headers as Record<string, string> | undefined)?.['X-FW-SHA256']);
      return adapter(c);
    };
    const r = await client.startModuleFirmware(0, new ArrayBuffer(8), { bank: '0x08030000', sha256: 'ab'.repeat(32) });
    expect(r.status).toBe('started');
    expect(seen).toHaveLength(2);
    expect(sent).toEqual(['ab'.repeat(32), undefined]);
  });

  it('a hub answer (400) is not retried', async () => {
    const { client, seen } = clientWith([{ status: 400, data: { message: 'REFUSED board' } }]);
    const r = await client.startModuleFirmware(0, new ArrayBuffer(8), { sha256: 'ab'.repeat(32) });
    expect(r).toMatchObject({ status: 'error', http: 400, message: 'REFUSED board' });
    expect(seen).toHaveLength(1);
  });
});
