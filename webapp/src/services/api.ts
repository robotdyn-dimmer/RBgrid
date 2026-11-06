/**
 * HTTP API client for RBGrid WebApp
 * Handles all HTTP communication with ESP32 device
 */

import axios, { AxiosInstance, AxiosRequestConfig, AxiosError, AxiosResponse } from 'axios';
import {
  BaseResponse,
  SystemConfigData,
  HardwareConfigData,
  TimeConfigData,
  TelemetryConfigData,
  WiFiConfigData,
  WiFiScanNetwork,
  WiFiScanStartData,
  WiFiScanResultsData,
  APIError,
  ConfigType,
  APIRequestOptions,
} from '../types';
import {
  UnitsConfigData,
} from '../types/device';
import {
  TariffConfigData,
} from '../types/tariff';
import {
  HistoryRequestParams,
  HistoryResponse,
  HistoryRecord,
  HistoryAll,
  HistorySinceResponse,
} from '../types/history';
import {
  SensorsListData,
  ModuleFwProgress,
  ModuleFwStart,
  UnitDeleteResult,
  CreateUnitRequest,
  UnitCreateResult,
  UnitRebindResult,
} from '../types/sensors';
import { TelemetryFrame } from '../types/telemetry-units';
import { WifiStatus, WifiSaveRequest } from '../types/network';
import {
  SystemConfig, SystemConfigEnvelope, TimeConfigView, TelemetryConfigView,
} from '../types/config';
import {
  demoSystemConfig, demoWifiStatus, demoTimeView, demoTelemetryView, demoTariffConfig,
  demoCreateUnit, demoDeleteUnit, demoSaveTariff, demoEnvelope, demoTelemetryFrame, demoAdjustUnitsConfig,
  demoHubEvents, demoHubVersion, demoModuleFwStart, demoModuleFw,
} from '../mock/demo';
import { TariffPlanConfigData } from '../types/tariff-plan';
import { mockConfig } from '../core/mocks';
import { HubEvent, HubEventsPage, HubVersion } from '../types/hubEvents';
import { I2cState, I2cScanResult, I2cWriteResult } from '../types/i2c';
import { getBaseUrl, isDemo, deviceSameOrigin } from './connection';
import { API_ENDPOINTS, HTTP_CONFIG, ENVIRONMENT, ERROR_CODES } from '../constants';
import { formatApiResponse } from './utils';

// ConfigType is imported from types

function isObj(v: unknown): v is Record<string, unknown> {
  return !!v && typeof v === 'object' && !Array.isArray(v);
}

/**
 * API client configuration
 */
export interface ApiClientConfig {
  baseURL: string;
  timeout?: number;
  headers?: Record<string, string>;
  retryConfig?: typeof HTTP_CONFIG.RETRY;
}

/**
 * API response interceptor data
 */
interface InterceptorData {
  config: AxiosRequestConfig;
  response?: AxiosResponse;
  error?: AxiosError;
}

// ===== Typed request errors =====
//
// Every rejection from the client is an `ApiRequestError` (a real `Error`, so callers'
// `e instanceof Error ? e.message : …` works) that ALSO keeps the HTTP facts: `status`,
// `headers`, `data`, and an AxiosError-compatible `response` view — so existing
// `err.response?.status` branches (createUnit/deleteUnit/useSensors) keep working.
// Specific statuses map to subclasses the UI can branch on with `instanceof`.

/** Lower-cased response headers (as axios exposes them). */
export type ResponseHeaders = Record<string, string>;

export interface ApiRequestErrorInit {
  code?: string;
  message?: string;
  status?: number;
  headers?: ResponseHeaders;
  data?: unknown;
  method?: string;
  url?: string;
  cause?: unknown;
}

/** Base error for every failed request made through RBGridApiClient. */

/** Shared history reads: in flight + fresh for HISTORY_TTL_MS (see getHistory). */
const HISTORY_TTL_MS = 10_000;
const historyCache = new Map<string, { p: Promise<unknown>; at: number; pending: boolean }>();
/** Tests / device switch: forget cached history. */
export const clearHistoryCache = (): void => historyCache.clear();
/**
 * Erased-flash history records (`ts`/`dur` 0xFFFFFFFF, `res` 255, `q` 65535) are slots the hub
 * never wrote — not hours (.139 18.10 after unbind + reuse). Dropped, else a «2106» row appears.
 */
export function dropErasedRecords<T>(data: T[]): T[] {
  return data.filter((r) => {
    const x = r as unknown as { ts?: number; res?: number };
    return !(x && (x.ts === 0xFFFFFFFF || x.res === 255));
  });
}

export class ApiRequestError extends Error implements APIError {
  readonly code: string;
  readonly details?: Record<string, any>;
  readonly timestamp: number;
  /** HTTP status (undefined for network errors / timeouts). */
  readonly status?: number;
  readonly headers: ResponseHeaders;
  /** Parsed response body, if any. */
  readonly data?: unknown;
  readonly method?: string;
  readonly url?: string;
  /** AxiosError-compatible view (only when the device answered). */
  readonly response?: { status: number; headers: ResponseHeaders; data: any };
  readonly cause?: unknown;

  constructor(init: ApiRequestErrorInit = {}) {
    super(init.message || 'Request failed');
    this.name = 'ApiRequestError';
    this.code = init.code || ERROR_CODES.UNKNOWN_ERROR;
    this.timestamp = Date.now();
    this.status = init.status;
    this.headers = init.headers ?? {};
    this.data = init.data;
    this.method = init.method;
    this.url = init.url;
    this.cause = init.cause;
    if (init.status !== undefined) {
      this.response = { status: init.status, headers: this.headers, data: init.data };
    }
    const d = init.data as Record<string, unknown> | undefined;
    if (d && typeof d === 'object' && d.details && typeof d.details === 'object') {
      this.details = d.details as Record<string, any>;
    }
  }

  /** Retry-After in seconds (numeric form only), if the device sent one and CORS exposes it. */
  get retryAfterSec(): number | undefined {
    return parseRetryAfter(this.headers['retry-after']);
  }
}

/**
 * Thrown when the device answers 503 during a stats-store migration (contract §7 / §3)
 * — body `{"error":"not_ready","storage_state":…,"progress":N}` (+ Retry-After: 30).
 * The UI should show "обновляем архив" with progress, not "истории нет". Never auto-retried
 * by the interceptor: the caller polls on its own cadence.
 */
export class StorageMigratingError extends ApiRequestError {
  readonly progress?: number;
  private readonly retryAfterOverride?: number;
  constructor(progress?: number, retryAfterSec?: number, init: ApiRequestErrorInit = {}) {
    super({ status: 503, ...init, message: 'Storage is migrating' });
    this.name = 'StorageMigratingError';
    this.progress = progress;
    this.retryAfterOverride = retryAfterSec;
  }
  get retryAfterSec(): number | undefined {
    return this.retryAfterOverride ?? parseRetryAfter(this.headers['retry-after']);
  }
}

/**
 * Thrown when an endpoint is not available in this firmware build (OSS). Canon (root T9,
 * 2026-10-12): the device answers 501 `{"error":"not_in_build","build":"oss"}`. We also treat
 * 405 as not-in-build defensively (it was a pre-T9 rudiment; now 405 means only literal
 * method-not-allowed). Either way the UI shows a "недоступно в сборке" card, not an error.
 */
export class NotInBuildError extends ApiRequestError {
  constructor(public readonly path: string, public readonly httpStatus: number, init: ApiRequestErrorInit = {}) {
    super({ status: httpStatus, ...init, message: `Endpoint not available in this build: ${path}` });
    this.name = 'NotInBuildError';
  }
}

/** 401 — no/invalid token on an auth-guarded route (`{"error":"unauthorized"}`). */
export class AuthRequiredError extends ApiRequestError {
  constructor(init: ApiRequestErrorInit = {}) {
    super({ status: 401, code: ERROR_CODES.AUTHENTICATION_FAILED, ...init, message: init.message || 'Authorization required' });
    this.name = 'AuthRequiredError';
  }
}

/** 403 — authenticated, but the role is insufficient (READ token on a write; T10 `admin_required`). */
export class AdminRequiredError extends ApiRequestError {
  constructor(init: ApiRequestErrorInit = {}) {
    super({ status: 403, code: ERROR_CODES.PERMISSION_DENIED, ...init, message: init.message || 'Admin token required' });
    this.name = 'AdminRequiredError';
  }
}

/** True for an HTTP status that means "endpoint absent in this build" (OSS): 501 (canon) or 405 (defensive). */
export function isNotInBuildStatus(status: number | undefined): boolean {
  return status === 501 || status === 405;
}

/** Parse a numeric Retry-After header (seconds). HTTP-date form is ignored (the device never sends it). */
export function parseRetryAfter(value: unknown): number | undefined {
  if (value === undefined || value === null || value === '') return undefined;
  const n = Number(value);
  return Number.isFinite(n) && n >= 0 ? n : undefined;
}

/** Methods the interceptor may retry (idempotent reads only — never POST/PUT/DELETE/PATCH). */
const RETRYABLE_METHODS = new Set(['get', 'head', 'options']);

/** Retry-After values above this are not waited out in-request — the error surfaces to the caller. */
const MAX_RETRY_AFTER_SEC = 10;

function lowerHeaders(h: unknown): ResponseHeaders {
  const out: ResponseHeaders = {};
  if (!h || typeof h !== 'object') return out;
  // AxiosHeaders exposes toJSON(); plain objects are iterated directly.
  const src = typeof (h as any).toJSON === 'function' ? (h as any).toJSON() : h;
  for (const [k, v] of Object.entries(src as Record<string, unknown>)) {
    if (v !== undefined && v !== null) out[k.toLowerCase()] = String(v);
  }
  return out;
}

/**
 * Convert an AxiosError into the typed error hierarchy. Exported for tests and for code
 * that calls axios directly but wants the same classification.
 */
export function toApiRequestError(error: AxiosError): ApiRequestError {
  if (error instanceof ApiRequestError) return error;
  const res = error.response;
  const method = error.config?.method?.toUpperCase();
  const url = error.config?.url;
  if (!res) {
    const timeout = error.code === 'ECONNABORTED' || error.code === 'ETIMEDOUT';
    return new ApiRequestError({
      code: timeout ? ERROR_CODES.TIMEOUT : ERROR_CODES.NETWORK_ERROR,
      message: timeout ? 'Request timeout' : 'Network error',
      method, url, cause: error,
    });
  }
  const status = res.status;
  const headers = lowerHeaders(res.headers);
  const data = res.data;
  const body = (data && typeof data === 'object' ? data : {}) as Record<string, any>;
  const message = String(body.message || body.error || error.message || `HTTP ${status}`);
  const init: ApiRequestErrorInit = {
    code: typeof body.code === 'string' ? body.code
      : typeof body.error_code === 'string' ? body.error_code
      : ERROR_CODES.UNKNOWN_ERROR,
    message, status, headers, data, method, url, cause: error,
  };
  if (isNotInBuildStatus(status)) return new NotInBuildError(url ?? '', status, init);
  if (status === 401) return new AuthRequiredError(init);
  if (status === 403) return new AdminRequiredError(init);
  if (status === 503 && typeof body.storage_state === 'string') {
    const progress = typeof body.progress === 'number' ? body.progress : undefined;
    return new StorageMigratingError(progress, parseRetryAfter(headers['retry-after']), init);
  }
  return new ApiRequestError(init);
}

/**
 * RBGrid API Client
 */
export class RBGridApiClient {
  private client: AxiosInstance;
  private retryConfig: ApiClientConfig['retryConfig'];
  private requestInterceptors: Array<(config: AxiosRequestConfig) => AxiosRequestConfig> = [];
  private responseInterceptors: Array<(response: AxiosResponse) => AxiosResponse> = [];

  constructor(config: ApiClientConfig) {
    this.retryConfig = config.retryConfig || HTTP_CONFIG.RETRY;
    
    // Create axios instance
    this.client = axios.create({
      baseURL: config.baseURL,
      timeout: config.timeout || HTTP_CONFIG.TIMEOUTS.DEFAULT,
      headers: {
        ...HTTP_CONFIG.HEADERS,
        ...config.headers,
      },
    });

    // Setup interceptors
    this.setupInterceptors();
  }

  /**
   * Setup request and response interceptors
   */
  private setupInterceptors(): void {
    // Request interceptor
    this.client.interceptors.request.use(
      (config) => {
        // Target the runtime-selected device (entered IP / same-origin), not a build-time hardcode.
        config.baseURL = getBaseUrl();

        // Add timestamp to requests
        config.params = {
          ...config.params,
          _t: Date.now(),
        };

        // Apply custom interceptors
        let finalConfig = config;
        for (const interceptor of this.requestInterceptors) {
          finalConfig = interceptor(finalConfig) as any;
        }

        return finalConfig as any;
      },
      (error) => {
        return Promise.reject(error);
      }
    );

    // Response interceptor
    this.client.interceptors.response.use(
      (response) => {
        // Apply custom interceptors
        let finalResponse = response;
        for (const interceptor of this.responseInterceptors) {
          finalResponse = interceptor(finalResponse);
        }

        return finalResponse;
      },
      async (error) => {
        // Cancellations are the caller's own doing — pass them through untouched.
        if (axios.isCancel(error)) return Promise.reject(error);

        // Retry policy (idempotent GETs only) — see retryDelayFor().
        const delay = this.retryDelayFor(error);
        if (delay !== null) {
          return this.retryRequest(error, delay);
        }

        // Typed error that keeps status/headers/data (B-3).
        return Promise.reject(toApiRequestError(error));
      }
    );
  }

  /**
   * Decide whether a failed request is retried, and after how long (ms). null = don't retry.
   *
   * Policy:
   *  - only idempotent methods (GET/HEAD/OPTIONS); POST/PUT/DELETE/PATCH are NEVER retried
   *    (units create, config writes, /wifi/save → reboot, reboot/restore must not run twice);
   *  - network errors / timeouts and 5xx are retried, EXCEPT 501 (not in build) and 4xx;
   *  - 503 with a storage-migration body is not retried (StorageMigratingError → caller polls);
   *  - 503 + Retry-After: wait exactly Retry-After when ≤ MAX_RETRY_AFTER_SEC, else give up
   *    immediately (the caller sees retryAfterSec on the error);
   *  - otherwise exponential backoff INITIAL_DELAY × BACKOFF_FACTOR^n, capped at MAX_DELAY,
   *    up to MAX_ATTEMPTS retries.
   */
  private retryDelayFor(error: AxiosError): number | null {
    const config = error.config;
    if (!config || !this.retryConfig) return null;

    const method = (config.method || 'get').toLowerCase();
    if (!RETRYABLE_METHODS.has(method)) return null;

    const retryCount = (config as any).__retryCount || 0;
    if (retryCount >= this.retryConfig.MAX_ATTEMPTS) return null;

    const res = error.response;
    if (res) {
      const status = res.status;
      if (status < 500 || status >= 600 || status === 501) return null;
      if (status === 503) {
        const body = res.data as Record<string, unknown> | undefined;
        if (body && typeof body === 'object' && typeof body.storage_state === 'string') return null;
        const retryAfter = parseRetryAfter(lowerHeaders(res.headers)['retry-after']);
        if (retryAfter !== undefined) {
          return retryAfter <= MAX_RETRY_AFTER_SEC ? retryAfter * 1000 : null;
        }
      }
    }

    return Math.min(
      this.retryConfig.INITIAL_DELAY * Math.pow(this.retryConfig.BACKOFF_FACTOR, retryCount),
      this.retryConfig.MAX_DELAY
    );
  }

  /**
   * Retry a failed (idempotent) request after `delay` ms.
   */
  private async retryRequest(error: AxiosError, delay: number): Promise<AxiosResponse> {
    const config = error.config!;
    (config as any).__retryCount = ((config as any).__retryCount || 0) + 1;

    await new Promise(resolve => setTimeout(resolve, delay));

    return this.client.request(config);
  }

  /**
   * Add request interceptor
   */
  public addRequestInterceptor(interceptor: (config: AxiosRequestConfig) => AxiosRequestConfig): void {
    this.requestInterceptors.push(interceptor);
  }

  /**
   * Add response interceptor
   */
  public addResponseInterceptor(interceptor: (response: AxiosResponse) => AxiosResponse): void {
    this.responseInterceptors.push(interceptor);
  }

  // ===== Configuration endpoints =====

  /**
   * Get system configuration
   */
  async getSystemConfig(options?: APIRequestOptions): Promise<BaseResponse<SystemConfigData>> {
    const response = await this.client.get<BaseResponse<SystemConfigData>>(
      API_ENDPOINTS.CONFIG.SYSTEM,
      { ...options }
    );
    return formatApiResponse<SystemConfigData>(response);
  }

  /**
   * Update system configuration
   */
  async updateSystemConfig(data: Partial<SystemConfigData> | Record<string, unknown>, options?: APIRequestOptions): Promise<unknown> {
    // Device POST requires the `{"system":{…}}` envelope (rbwebserver.c: cJSON_GetObjectItem(root,"system"),
    // else 400 MISSING_SYSTEM_SECTION). Partial inner objects are fine — absent fields keep their value.
    // The old validator demanded a `system_settings` key that no firmware ever sent (B-2) — removed.
    const body = isObj((data as Record<string, unknown>).system) ? data : { system: data };
    return this.postConfig(API_ENDPOINTS.CONFIG.SYSTEM, body, options);
  }

  /**
   * POST an already-shaped config body (the exact envelope the device handler parses).
   * No client-side shape validation — the device is the authority and answers structured 400s.
   * Never retried by the interceptor (POST).
   */
  async postConfig(path: string, body: unknown, options?: APIRequestOptions): Promise<unknown> {
    const response = await this.client.post(
      path,
      body,
      { ...options, timeout: options?.timeout ?? HTTP_CONFIG.TIMEOUTS.CONFIG_WRITE }
    );
    return response.data;
  }

  /**
   * Get hardware configuration
   */
  async getHardwareConfig(options?: APIRequestOptions): Promise<BaseResponse<HardwareConfigData>> {
    const response = await this.client.get<BaseResponse<HardwareConfigData>>(
      API_ENDPOINTS.CONFIG.HARDWARE,
      { ...options }
    );
    return formatApiResponse<HardwareConfigData>(response);
  }

  /**
   * Get time configuration
   */
  async getTimeConfig(options?: APIRequestOptions): Promise<BaseResponse<TimeConfigData>> {
    // Demo answers from the mock — no request to the page's own origin (UX audit HEU-05).
    if (isDemo()) return demoEnvelope('time_config', demoTimeView() as unknown as TimeConfigData);
    const response = await this.client.get<BaseResponse<TimeConfigData>>(
      API_ENDPOINTS.CONFIG.TIME,
      { ...options }
    );
    return formatApiResponse<TimeConfigData>(response);
  }

  /**
   * Update time configuration
   */
  async updateTimeConfig(_data: Partial<TimeConfigData>, _options?: APIRequestOptions): Promise<never> {
    // The firmware registers time-config as GET-only (rbwebserver.c) — there is no POST route.
    // Fail before the network instead of collecting a 405.
    throw new Error('time-config is read-only on the device (no POST route); use triggerTimeSync()');
  }

  /**
   * Get units configuration
   */
  async getUnitsConfig(options?: APIRequestOptions): Promise<BaseResponse<UnitsConfigData>> {
    if (isDemo()) return demoEnvelope('units_config', demoAdjustUnitsConfig(mockConfig('units')) as unknown as UnitsConfigData);
    const response = await this.client.get<BaseResponse<UnitsConfigData>>(
      API_ENDPOINTS.CONFIG.UNITS,
      { ...options }
    );
    return formatApiResponse<UnitsConfigData>(response);
  }

  /**
   * Update units configuration
   */
  async updateUnitsConfig(data: Partial<UnitsConfigData> | Record<string, unknown>, options?: APIRequestOptions): Promise<unknown> {
    // Device POST requires `{"type":"units_config","data":{"units":[…]}}` (rbwebserver_api.c),
    // body ≤ 8 KB (CONFIG_COMPACT_SIZE). Accept either the full envelope or the inner data.
    const d = data as Record<string, unknown>;
    const body = d.type === 'units_config' && isObj(d.data) ? data : { type: 'units_config', data };
    return this.postConfig(API_ENDPOINTS.CONFIG.UNITS, body, options);
  }

  /**
   * Get tariffs configuration
   */
  async getTariffsConfig(options?: APIRequestOptions): Promise<BaseResponse<TariffConfigData>> {
    const response = await this.client.get<BaseResponse<TariffConfigData>>(
      API_ENDPOINTS.CONFIG.TARIFFS,
      { ...options }
    );
    return formatApiResponse<TariffConfigData>(response);
  }

  /**
   * Update tariffs configuration
   */
  async updateTariffsConfig(data: Partial<TariffConfigData> | Record<string, unknown>, options?: APIRequestOptions): Promise<unknown> {
    // Device POST requires `{"type":"tariff_plan_list_config","data":{"list":[…]}}` (saved verbatim).
    const d = data as Record<string, unknown>;
    const body = d.type === 'tariff_plan_list_config' && isObj(d.data) ? data : { type: 'tariff_plan_list_config', data };
    return this.postConfig(API_ENDPOINTS.CONFIG.TARIFFS, body, options);
  }

  /**
   * Get telemetry configuration
   */
  async getTelemetryConfig(options?: APIRequestOptions): Promise<BaseResponse<TelemetryConfigData>> {
    const response = await this.client.get<BaseResponse<TelemetryConfigData>>(
      API_ENDPOINTS.CONFIG.TELEMETRY,
      { ...options }
    );
    return formatApiResponse<TelemetryConfigData>(response);
  }

  /**
   * Update telemetry configuration
   */
  async updateTelemetryConfig(data: Partial<TelemetryConfigData> | Record<string, unknown>, options?: APIRequestOptions): Promise<unknown> {
    // The device's telemetry-config POST accepts ONLY `{"profile":"lean"|"full"}` (body < 128 B);
    // every other telemetry field is read-only over REST.
    const profile = (data as Record<string, unknown>).profile;
    if (profile !== 'lean' && profile !== 'full') {
      throw new Error('telemetry-config POST accepts only profile = "lean" | "full"');
    }
    return this.postConfig(API_ENDPOINTS.CONFIG.TELEMETRY, { profile }, options);
  }

  /**
   * Get WiFi configuration
   */
  async getWiFiConfig(options?: APIRequestOptions): Promise<BaseResponse<WiFiConfigData>> {
    const response = await this.client.get<BaseResponse<WiFiConfigData>>(
      API_ENDPOINTS.CONFIG.WIFI,
      { ...options }
    );
    return formatApiResponse<WiFiConfigData>(response);
  }

  /**
   * Update WiFi configuration
   */
  async updateWiFiConfig(_data: Partial<WiFiConfigData>, _options?: APIRequestOptions): Promise<never> {
    // wifi-config is GET-only on the device; credentials go through POST /wifi/save (reboots) — saveWifi().
    throw new Error('wifi-config is read-only on the device; use saveWifi() (POST /wifi/save, reboots)');
  }

  /**
   * Start an async WiFi scan.
   * Returns immediately (firmware replies HTTP 202, data.status="scanning").
   * Idempotent: calling while a scan is already running does not start a second.
   */
  async startWiFiScan(options?: APIRequestOptions): Promise<BaseResponse<WiFiScanStartData>> {
    const response = await this.client.get<BaseResponse<WiFiScanStartData>>(
      API_ENDPOINTS.WIFI.SCAN,
      { ...options }
    );
    return formatApiResponse<WiFiScanStartData>(response);
  }

  /**
   * Fetch the latest WiFi scan results (HTTP 200 always; branch on data.status).
   */
  async getWiFiScanResults(options?: APIRequestOptions): Promise<BaseResponse<WiFiScanResultsData>> {
    const response = await this.client.get<BaseResponse<WiFiScanResultsData>>(
      API_ENDPOINTS.WIFI.SCAN_RESULTS,
      { ...options }
    );
    return formatApiResponse<WiFiScanResultsData>(response);
  }

  /**
   * Run a full WiFi scan: start it, then poll results until done.
   * Polls /wifi/scan/results every `pollIntervalMs` (default 500ms) until
   * status !== "scanning", giving up after `timeoutMs` (default 6000ms).
   * Pass `options.signal` to cancel on component unmount.
   *
   * @returns the discovered networks (empty array on a "error"/empty result)
   * @throws on scan timeout or if the request is aborted
   */
  async scanWiFiNetworks(
    options?: APIRequestOptions & { pollIntervalMs?: number; timeoutMs?: number }
  ): Promise<WiFiScanNetwork[]> {
    const pollIntervalMs = options?.pollIntervalMs ?? 500;
    const timeoutMs = options?.timeoutMs ?? 6000;
    const signal = options?.signal;

    await this.startWiFiScan(options);

    const deadline = this.now() + timeoutMs;
    // Poll until firmware reports a terminal status or we hit the deadline.
    for (;;) {
      await this.delay(pollIntervalMs, signal);
      const { data } = await this.getWiFiScanResults(options);

      if (data.status === 'done' || data.status === 'error') {
        return data.networks;
      }
      if (this.now() >= deadline) {
        throw new Error(`WiFi scan timed out after ${timeoutMs}ms`);
      }
    }
  }

  /** Monotonic-ish clock indirection (kept private so callers don't read Date). */
  private now(): number {
    return Date.now();
  }

  /** Promise delay that rejects early if the abort signal fires. */
  private delay(ms: number, signal?: AbortSignal): Promise<void> {
    return new Promise<void>((resolve, reject) => {
      if (signal?.aborted) {
        reject(new DOMException('Aborted', 'AbortError'));
        return;
      }
      const timer = setTimeout(() => {
        signal?.removeEventListener('abort', onAbort);
        resolve();
      }, ms);
      const onAbort = () => {
        clearTimeout(timer);
        reject(new DOMException('Aborted', 'AbortError'));
      };
      signal?.addEventListener('abort', onAbort, { once: true });
    });
  }

  /**
   * Get all configurations
   */
  async getAllConfigs(options?: APIRequestOptions): Promise<Record<ConfigType, BaseResponse<any>>> {
    const configTypes: ConfigType[] = ['system', 'hardware', 'time', 'units', 'tariffs', 'telemetry', 'wifi'];
    const promises = configTypes.map(type => this.getConfig(type, options));
    const results = await Promise.allSettled(promises);

    const configs: Record<string, BaseResponse<any>> = {};
    configTypes.forEach((type, index) => {
      const result = results[index];
      if (result.status === 'fulfilled') {
        configs[type] = result.value;
      }
    });

    return configs as Record<ConfigType, BaseResponse<any>>;
  }

  /**
   * Get configuration by type
   */
  async getConfig<T = any>(type: ConfigType, options?: APIRequestOptions): Promise<BaseResponse<T>> {
    switch (type) {
      case 'system':
        return this.getSystemConfig(options) as Promise<BaseResponse<T>>;
      case 'hardware':
        return this.getHardwareConfig(options) as Promise<BaseResponse<T>>;
      case 'time':
        return this.getTimeConfig(options) as Promise<BaseResponse<T>>;
      case 'units':
        return this.getUnitsConfig(options) as Promise<BaseResponse<T>>;
      case 'tariffs':
        return this.getTariffsConfig(options) as Promise<BaseResponse<T>>;
      case 'telemetry':
        return this.getTelemetryConfig(options) as Promise<BaseResponse<T>>;
      case 'wifi':
        return this.getWiFiConfig(options) as Promise<BaseResponse<T>>;
      default:
        throw new Error(`Unknown configuration type: ${type}`);
    }
  }

  // ===== Device management endpoints =====

  /**
   * Reboot device
   */
  async rebootDevice(options?: APIRequestOptions): Promise<{ success: boolean; message: string }> {
    if (isDemo()) return { success: true, message: 'demo: restart simulated' };
    const response = await this.client.post<{ success: boolean; message: string }>(
      API_ENDPOINTS.DEVICE.REBOOT,
      {},
      { ...options, timeout: HTTP_CONFIG.TIMEOUTS.DEVICE_COMMAND }
    );
    return response.data;
  }

  /**
   * Reset device to factory defaults
   */
  async resetDevice(confirm: boolean = false, options?: APIRequestOptions): Promise<{ success: boolean; message: string }> {
    if (!confirm) {
      throw new Error('Device reset requires confirmation');
    }
    if (isDemo()) return { success: true, message: 'demo: reset simulated' };

    const response = await this.client.post<{ success: boolean; message: string }>(
      API_ENDPOINTS.DEVICE.RESTORE,
      { confirm: true },
      { ...options, timeout: HTTP_CONFIG.TIMEOUTS.DEVICE_COMMAND }
    );
    return response.data;
  }

  /**
   * v1.1.2 factory reset (root 01d14d6): POST /api/system/factory-reset {history} → 200
   * {status:"ok",history,rebooting:true}; the hub restarts ~1 s later and comes back as on first boot
   * (its own access point + portal) — the app loses it. A 4xx comes back as {status:"error",message}.
   */
  async factoryReset(history: 'erase' | 'keep', options?: APIRequestOptions): Promise<{ status: 'ok' | 'error'; message?: string }> {
    if (isDemo()) return { status: 'ok' };
    try {
      await this.client.post(API_ENDPOINTS.DEVICE.FACTORY_RESET, { history }, { ...options, timeout: HTTP_CONFIG.TIMEOUTS.DEVICE_COMMAND });
      return { status: 'ok' };
    } catch (err) {
      if (err instanceof ApiRequestError && err.status !== undefined && err.status < 500
        && !(err instanceof AuthRequiredError) && !(err instanceof AdminRequiredError)) {
        const d = (err.data && typeof err.data === 'object' ? err.data : {}) as { message?: string; error?: string };
        return { status: 'error', message: d.message ?? d.error ?? err.message };
      }
      throw err;
    }
  }

  // ===== Network / WiFi =====

  /** Current WiFi runtime status (GET /wifi/status). */
  async getWifiStatus(options?: APIRequestOptions): Promise<WifiStatus> {
    if (isDemo()) return demoWifiStatus();
    const response = await this.client.get<WifiStatus>(API_ENDPOINTS.WIFI.STATUS, { ...options });
    return response.data;
  }

  /** Save WiFi credentials (POST /wifi/save) — the device reboots ~1s after. */
  async saveWifi(
    body: WifiSaveRequest,
    options?: APIRequestOptions
  ): Promise<{ status?: string; success?: boolean; message?: string }> {
    const response = await this.client.post<{ status?: string; success?: boolean; message?: string }>(
      API_ENDPOINTS.WIFI.SAVE,
      body,
      { ...options, timeout: HTTP_CONFIG.TIMEOUTS.DEVICE_COMMAND }
    );
    return response.data;
  }

  // ===== Realigned config domains (per-domain envelopes — root 2026-06-20) =====

  /** GET /api/system/system-config → `{ system: {...} }` (flat, NOT BaseResponse). */
  async getSystemSettings(options?: APIRequestOptions): Promise<SystemConfig> {
    if (isDemo()) return demoSystemConfig();
    const response = await this.client.get<SystemConfigEnvelope>(
      API_ENDPOINTS.CONFIG.SYSTEM,
      { ...options }
    );
    return response.data.system;
  }

  /** POST /api/system/system-config — body is the same `{ system: {...} }` envelope. */
  async saveSystemSettings(system: SystemConfig, options?: APIRequestOptions): Promise<unknown> {
    const response = await this.client.post(
      API_ENDPOINTS.CONFIG.SYSTEM,
      { system },
      { ...options, timeout: HTTP_CONFIG.TIMEOUTS.CONFIG_WRITE }
    );
    return response.data;
  }

  /** GET /api/system/time-config (BaseResponse, data:{config,statistics}). Read-only view. */
  async getTimeSettings(options?: APIRequestOptions): Promise<TimeConfigView> {
    if (isDemo()) return demoTimeView();
    const response = await this.client.get<BaseResponse<TimeConfigView>>(
      API_ENDPOINTS.CONFIG.TIME,
      { ...options }
    );
    return formatApiResponse<TimeConfigView>(response).data;
  }

  /**
   * POST /api/system/time-sync is an ACTION endpoint (rbwebserver.c, admin token):
   *   {action:"sync_time", data:{source:"SNTP"}}                        → force an SNTP sync now;
   *   {action:"sync_time", data:{source:"MANUAL_WEB", timestamp, timezone?}} → set the clock from
   *       this browser (unix SECONDS), then SNTP;
   *   {action:"save_config", data:{config:{…time config…}}}           → persist; timezone applies
   *       live, the answer may carry requires_reboot for the rest.
   * (An empty body — what this method used to send — is rejected with 400 «Missing 'action'».)
   */
  private async timeSyncAction(body: unknown, options?: APIRequestOptions): Promise<Record<string, unknown>> {
    const response = await this.client.post(
      API_ENDPOINTS.DEVICE.TIME_SYNC,
      body,
      { ...options, timeout: HTTP_CONFIG.TIMEOUTS.DEVICE_COMMAND }
    );
    return (response.data ?? {}) as Record<string, unknown>;
  }

  /** Force an SNTP sync now. */
  async triggerTimeSync(options?: APIRequestOptions): Promise<Record<string, unknown>> {
    if (isDemo()) return { status: 'OK' };
    return this.timeSyncAction({ action: 'sync_time', data: { source: 'SNTP' } }, options);
  }

  /** Set the device clock from this browser's clock (no internet / no NTP on the LAN). */
  async syncTimeFromBrowser(timezone?: string, options?: APIRequestOptions): Promise<Record<string, unknown>> {
    if (isDemo()) return { status: 'OK' };
    const data: Record<string, unknown> = { source: 'MANUAL_WEB', timestamp: Math.floor(Date.now() / 1000) };
    if (timezone) data.timezone = timezone;
    return this.timeSyncAction({ action: 'sync_time', data }, options);
  }

  /** Persist the time configuration (timezone applies live; may answer requires_reboot). */
  async saveTimeConfig(config: Record<string, unknown>, options?: APIRequestOptions): Promise<Record<string, unknown>> {
    if (isDemo()) return { status: 'OK', requires_reboot: false };
    return this.timeSyncAction({ action: 'save_config', data: { config } }, options);
  }

  /** GET /api/system/telemetry-config (BaseResponse). Read-only view (POST is firmware gap #3). */
  async getTelemetrySettings(options?: APIRequestOptions): Promise<TelemetryConfigView> {
    if (isDemo()) return demoTelemetryView();
    const response = await this.client.get<BaseResponse<TelemetryConfigView>>(
      API_ENDPOINTS.CONFIG.TELEMETRY,
      { ...options }
    );
    return formatApiResponse<TelemetryConfigView>(response).data;
  }

  /**
   * GET /api/system/tariff-plan-config — full BaseResponse{data:{list,tariff_settings?}}.
   * Returns the FULL envelope (POST sends the same envelope back, like units-config).
   */
  async getTariffPlans(options?: APIRequestOptions): Promise<BaseResponse<TariffPlanConfigData>> {
    if (isDemo()) return demoTariffConfig();
    const response = await this.client.get<BaseResponse<TariffPlanConfigData>>(
      API_ENDPOINTS.CONFIG.TARIFFS,
      { ...options }
    );
    return formatApiResponse<TariffPlanConfigData>(response);
  }

  /** POST /api/system/tariff-plan-config — body is the full envelope returned by GET (mutated). */
  async saveTariffPlans(
    full: BaseResponse<TariffPlanConfigData>,
    options?: APIRequestOptions
  ): Promise<unknown> {
    if (isDemo()) { demoSaveTariff(full); return { status: 'success' }; }
    const response = await this.client.post(
      API_ENDPOINTS.CONFIG.TARIFFS,
      full,
      { ...options, timeout: HTTP_CONFIG.TIMEOUTS.CONFIG_WRITE }
    );
    return response.data;
  }

  // ===== Live telemetry snapshot =====

  /**
   * Fetch one live telemetry frame (compact shape, same as the /ws-telemetry push).
   * Use for the dashboard's FIRST frame while the WS connects, then switch to the WS stream.
   * Flat response (not a BaseResponse).
   */
  async getLatestTelemetry(options?: APIRequestOptions): Promise<TelemetryFrame> {
    if (isDemo()) return demoTelemetryFrame();
    const response = await this.client.get<TelemetryFrame>(
      API_ENDPOINTS.TELEMETRY_LATEST,
      // Frame v2 per request (canon 274a569c); older hubs ignore it and answer v1.
      { ...options, params: { v: 2 } }
    );
    return response.data;
  }

  // ===== Phase 4 config-lifecycle: sensor discovery + unit deletion =====

  /**
   * List discovered sensors (rbAmp channels on I2C) with hardware presence.
   * BaseResponse-enveloped (type "sensors_list"). Variant (a): bound sensors only —
   * every entry currently has a non-null unit_id; unbound (unit_id:null) arrives with
   * variant (b). Use for the "configure sensors → create units" devices screen.
   */
  async getSensors(options?: APIRequestOptions): Promise<BaseResponse<SensorsListData>> {
    // Device returns the sensors array at the TOP LEVEL (key `sensors`), inside the
    // type/version/device_id envelope but NOT under `data` (verified on hw 2026-06-29).
    // Tolerate both shapes instead of requiring `.data` (formatApiResponse would throw).
    const response = await this.client.get<Record<string, unknown>>(API_ENDPOINTS.SENSORS, { ...options });
    const body = (response.data ?? {}) as Record<string, unknown>;
    // `modules[]` (delta №5) sits next to `sensors` at the top level — keep it (UX audit PNT-02:
    // dropping it hid the «Модули rbAmp» block on every real hub).
    const data = (body.data ?? {
      sensors: body.sensors ?? [],
      ...(Array.isArray(body.modules) ? { modules: body.modules } : {}),
    }) as SensorsListData;
    // v1.1.2 bus-stall flag sits at the top level next to sensors (root 03.11).
    if (typeof body.bus_stalled_s === 'number' && data.bus_stalled_s === undefined) data.bus_stalled_s = body.bus_stalled_s;
    if (typeof body.bus_line_low_s === 'number' && data.bus_line_low_s === undefined) data.bus_line_low_s = body.bus_line_low_s;
    return {
      type: (body.type as string) ?? 'sensors_list',
      version: (body.version as string) ?? '2.0',
      device_id: (body.device_id as string) ?? 'unknown',
      timestamp_local: (body.timestamp_local as number) ?? 0,
      timestamp_ntp: (body.timestamp_ntp as number) ?? 0,
      data,
    };
  }

  /**
   * Module firmware through the hub (root 534ded9e): POST the prepared `.ota` image for the
   * module's INACTIVE bank; 202 → started. 409 (one at a time) and 400/404 REFUSED come back as
   * `{status:'error', reason, message}` for the dialog; auth and server errors re-throw.
   */
  async startModuleFirmware(module: number, image: ArrayBuffer, q: { bank?: string; board?: string; sha256?: string }): Promise<ModuleFwStart> {
    if (isDemo()) return demoModuleFwStart(module, q.bank, image.byteLength);
    const params = new URLSearchParams();
    if (q.bank) params.set('bank', q.bank);
    if (q.board) params.set('board', q.board);
    const post = (sha256?: string) => this.client.post<Record<string, unknown>>(
      `${API_ENDPOINTS.MODULES}/${module}/firmware${params.toString() ? `?${params}` : ''}`,
      image,
      { headers: { 'Content-Type': 'application/octet-stream', ...(sha256 ? { 'X-FW-SHA256': sha256 } : {}) }, timeout: HTTP_CONFIG.TIMEOUTS.FILE_UPLOAD },
    );
    try {
      let response;
      try {
        response = await post(q.sha256);
      } catch (err) {
        // Hubs before 0d67ae6f don't list X-FW-* in their CORS preflight: cross-origin the browser
        // drops the upload unsent («Network error») — send it once more without the hash.
        if (!q.sha256 || deviceSameOrigin() || !(err instanceof ApiRequestError) || err.code !== ERROR_CODES.NETWORK_ERROR) throw err;
        response = await post();
      }
      return { status: 'started', ...(response.data as object) } as ModuleFwStart;
    } catch (err) {
      if (err instanceof ApiRequestError && err.status !== undefined && err.status < 500
        && !(err instanceof AuthRequiredError) && !(err instanceof AdminRequiredError)) {
        const d = (err.data ?? {}) as { message?: string; reason?: string; error?: string };
        return { status: 'error', http: err.status, reason: d.reason ?? d.error, message: d.message ?? err.message };
      }
      throw err;
    }
  }

  /** Progress of a module update (GET /api/v1/modules/<m>/firmware). */
  async getModuleFirmware(module: number, options?: APIRequestOptions): Promise<ModuleFwProgress> {
    if (isDemo()) return demoModuleFw(module);
    const response = await this.client.get<ModuleFwProgress>(`${API_ENDPOINTS.MODULES}/${module}/firmware`, { ...options });
    return response.data;
  }

  /** The hub's rbAmp fleet on I2C (GET /api/v1/i2c): buses, configured modules, the module limit. */
  async getI2c(options?: APIRequestOptions): Promise<I2cState> {
    const response = await this.client.get<I2cState>(API_ENDPOINTS.I2C, { ...options });
    return response.data;
  }

  /** A structured 4xx of /api/v1/i2c ({status:"error",code,message}) → an inline result; the rest re-throws. */
  private i2cError(err: unknown): { status: 'error'; code: string; message: string; http?: number } {
    if (err instanceof ApiRequestError && err.status !== undefined && err.status < 500
      && !(err instanceof AuthRequiredError) && !(err instanceof AdminRequiredError)) {
      const d = (err.data ?? {}) as { code?: string; message?: string };
      return { status: 'error', code: d.code ?? String(err.status), message: d.message ?? err.message, http: err.status };
    }
    throw err;
  }

  /** Who answers on a bus (POST /api/v1/i2c/scan, 0.16 s for one module on .139). 409 BUS_STUCK = a line held low (an unpowered / Modbus-mode / miswired module). */
  async scanI2c(bus: number): Promise<I2cScanResult> {
    try {
      const response = await this.client.post<{ devices?: unknown }>(`${API_ENDPOINTS.I2C}/scan`, { bus }, { timeout: 20_000 });
      return { status: 'ok', devices: Array.isArray(response.data?.devices) ? response.data.devices : [] };
    } catch (err) { return this.i2cError(err); }
  }

  /** Add a scanned rbAmp module (POST /api/v1/i2c/modules); it joins at once, its channels become free sensors. */
  async addI2cModule(bus: number, addr: number): Promise<I2cWriteResult> {
    try {
      const response = await this.client.post<I2cState>(`${API_ENDPOINTS.I2C}/modules`, { bus, addr }, { timeout: 15_000 });
      return { ...response.data, status: 'ok' };
    } catch (err) { return this.i2cError(err); }
  }

  /** Move a module to another address (POST /api/v1/i2c/modules/<m>/address) — saved in the module, UID-checked. */
  async setI2cModuleAddr(module: number, addr: number): Promise<I2cWriteResult> {
    try {
      const response = await this.client.post<I2cState>(`${API_ENDPOINTS.I2C}/modules/${module}/address`, { addr }, { timeout: 15_000 });
      return { ...response.data, status: 'ok' };
    } catch (err) { return this.i2cError(err); }
  }

  /**
   * Create / re-bind a unit from an (unbound) sensor (POST /api/v1/units). Inverse of
   * deleteUnit. The sensor is identified by its addressing; unit_id is firmware-assigned
   * and returned on success (201 {status:"ok",unit_id,persist}).
   *
   * Known client errors (400 bad body / sensor-not-found / unsupported-iface, 409 already
   * bound) are returned as {status:"error",message} for inline UI handling. Server errors
   * (507 "unit network full") and anything unexpected re-throw.
   *
   * Only rbAmp (i2c) sensors exist — the product has no built-in ADC (operator 18.10).
   */
  async createUnit(body: CreateUnitRequest, options?: APIRequestOptions): Promise<UnitCreateResult> {
    if (isDemo()) return demoCreateUnit(body);
    try {
      const response = await this.client.post<UnitCreateResult>(
        API_ENDPOINTS.UNITS,
        body,
        { ...options }
      );
      return response.data;
    } catch (err) {
      // Structured 4xx ({status:"error",message}) → inline result; the interceptor keeps
      // status/data on ApiRequestError (401/403 stay typed throws: auth, not a body error).
      if (err instanceof ApiRequestError && err.status !== undefined && err.status < 500
        && !(err instanceof AuthRequiredError) && !(err instanceof AdminRequiredError)
        && err.data && typeof err.data === 'object') {
        // {status:"error",message} and the T7 409s {code} / {error} alike.
        const d = err.data as UnitCreateResult & { error?: string };
        if (d.status === 'error' || d.code || d.error) return { ...d, status: 'error', code: d.code ?? d.error, message: d.message ?? d.error };
      }
      throw err;
    }
  }

  /**
   * Regroup a module as the input's three-phase source (v1.1.2, root f06ed69):
   * POST /api/v1/units/<input hex>/rebind {module, ch:254} → 200 {grouped, requires_reboot}. The
   * hub's 4xx (409 SENSOR_BOUND, 400 NOT_PHASE_MODULE) come back as {status:"error",code,message}.
   */
  async rebindInputGroup(inputHex: string, module: number, options?: APIRequestOptions): Promise<UnitRebindResult> {
    return this.rebindUnit(inputHex, module, 254, options);
  }

  /**
   * Move a point to another channel of a module (fw 1.1.5, root u3i3-per-phase-points): ch 0..2 splits
   * a grouped input — the point keeps that phase, the others turn into free sensors; ch 254 groups it
   * back. Always `requires_reboot`.
   */
  async rebindUnit(hexId: string, module: number, ch: number, options?: APIRequestOptions): Promise<UnitRebindResult> {
    try {
      const response = await this.client.post<UnitRebindResult>(
        `${API_ENDPOINTS.UNITS}/${hexId}/rebind`, { module, ch }, { ...options });
      return { ...response.data, status: 'ok' };
    } catch (err) {
      if (err instanceof ApiRequestError && err.status !== undefined && err.status < 500
        && !(err instanceof AuthRequiredError) && !(err instanceof AdminRequiredError)) {
        const d = (err.data && typeof err.data === 'object' ? err.data : {}) as UnitRebindResult & { error?: string };
        return { status: 'error', code: d.code ?? d.error, message: d.message ?? d.error ?? err.message };
      }
      throw err;
    }
  }

  /**
   * Delete a unit by hex id (REST DELETE /api/v1/units/{hexid}). Cascade = unit-only:
   * the unit leaves telemetry + sensor-list; the underlying sensor/fleet config is untouched.
   *
   * Returns the flat result on success (200 {status:"ok",removed,persist}). Known structured
   * 4xx errors (404 not-found / idempotent repeat, 409 has-children, 400 bad-id) are returned
   * as {status:"error",message} so the UI can branch; unexpected failures re-throw.
   *
   * NOTE: an rbAmp unit re-appears after reboot (auto-created from the fleet config). For the unit count, re-query getSensors/units-config.
   */
  async deleteUnit(hexId: string, options?: APIRequestOptions): Promise<UnitDeleteResult> {
    if (isDemo()) return demoDeleteUnit(hexId);
    try {
      const response = await this.client.delete<UnitDeleteResult>(
        `${API_ENDPOINTS.UNITS}/${hexId}`,
        { ...options }
      );
      return response.data;
    } catch (err) {
      if (err instanceof ApiRequestError && err.status !== undefined && err.status < 500
        && !(err instanceof AuthRequiredError) && !(err instanceof AdminRequiredError)
        && (err.data as UnitDeleteResult | undefined)?.status === 'error') {
        return err.data as UnitDeleteResult;
      }
      throw err;
    }
  }

  // ===== Statistics history (FINAL contract — /api/v1/history) =====

  /**
   * Fetch aggregated statistics history (FINAL contract, flat response).
   *
   * Each record is one aggregated period bucket. Results are NEWEST-FIRST
   * (`data[0]` = freshest) — reverse client-side for an ascending time axis.
   * Paginate via offset/limit (limit ≤ 720) using `meta.has_more` to load older
   * buckets. `meta.total_available` is the real stored count, not ring capacity.
   *
   * NOTE: parses the flat `{status,request,meta,data}` shape directly. This is a
   * data-endpoint, NOT the BaseResponse envelope, so it bypasses formatApiResponse.
   */
  getHistory<T = HistoryRecord>(
    params: HistoryRequestParams,
    options?: APIRequestOptions
  ): Promise<HistoryResponse<T>> {
    // Identical reads share one request and a 10 s answer (3φ audit 16.10: «Экономика» sent ~20
    // history GETs on load, each twice — the hub's poll stalled under several tabs). Closed hours
    // don't change in 10 s; the running hour isn't a record.
    const key = `${getBaseUrl()}|${params.unit_id}|${params.period}|${params.type}|${params.offset ?? 0}|${params.limit ?? 720}|${params.from ?? ''}|${params.to ?? ''}`;
    const now = Date.now();
    const hit = historyCache.get(key);
    if (hit && (hit.pending || now - hit.at < HISTORY_TTL_MS)) return hit.p as Promise<HistoryResponse<T>>;
    const p = this.fetchHistory<T>(params, options);
    const entry = { p: p as Promise<unknown>, at: now, pending: true };
    historyCache.set(key, entry);
    p.then(() => { entry.pending = false; entry.at = Date.now(); }, () => { historyCache.delete(key); });
    if (historyCache.size > 300) historyCache.delete(historyCache.keys().next().value as string);
    return p;
  }

  private async fetchHistory<T = HistoryRecord>(
    params: HistoryRequestParams,
    options?: APIRequestOptions
  ): Promise<HistoryResponse<T>> {
    const response = await this.client.get<unknown>(
      API_ENDPOINTS.HISTORY,
      {
        ...options,
        params: {
          unit_id: params.unit_id,
          period: params.period,
          type: params.type,
          offset: params.offset ?? 0,
          limit: params.limit ?? 720,
          ...(params.from !== undefined ? { from: params.from } : {}),
          ...(params.to !== undefined ? { to: params.to } : {}),
        },
        timeout: HTTP_CONFIG.TIMEOUTS.STATISTICS,
      }
    );
    // Three envelopes (root 2026-06-23): legacy single-type {status,request,meta,data};
    // single-type+from/to {v,unit,period,data:[]}; type=all {v,unit,period,data:{…}}.
    const raw = response.data as Record<string, unknown> | null;
    if (raw && typeof raw.status === 'string') {
      if (raw.status !== 'ok') throw new Error(`History request failed: ${raw.status}`);
      const r = raw as unknown as HistoryResponse<T>;
      return Array.isArray(r.data) ? { ...r, data: dropErasedRecords(r.data) } : r;
    }
    if (raw && Array.isArray(raw.data)) {
      const data = dropErasedRecords(raw.data as T[]); // windowed single-type envelope → normalize
      return {
        status: 'ok',
        request: {
          unit_id: params.unit_id, period: params.period, type: params.type,
          offset: params.offset ?? 0, limit: params.limit ?? 720,
        },
        meta: { total_available: data.length, returned: data.length, has_more: false },
        data,
      };
    }
    throw new Error('History request failed: unexpected response shape');
  }

  /**
   * Incremental history via the `since` cursor (T9 contract §3, live ≥7e0b1301).
   * Returns strictly `ts > since` ASC, with `next_since`/`has_more`/`total_available`.
   * The client keeps `next_since` and tops up while `has_more`. Single-type only (no type=all).
   *
   * During a stats-store migration the device answers 503 + Retry-After; we surface that as
   * a recognizable `StorageMigratingError` so the UI can say "обновляем архив" (not "истории нет").
   */
  async getHistorySince<T = HistoryRecord>(
    params: HistoryRequestParams & { since: number },
    options?: APIRequestOptions,
  ): Promise<HistorySinceResponse<T>> {
    try {
      const response = await this.client.get<HistorySinceResponse<T>>(
        API_ENDPOINTS.HISTORY,
        {
          ...options,
          params: {
            unit_id: params.unit_id,
            period: params.period,
            type: params.type,
            since: params.since,
            limit: params.limit ?? 720,
            ...(params.include_unsynced !== undefined ? { include_unsynced: params.include_unsynced } : {}),
          },
          timeout: HTTP_CONFIG.TIMEOUTS.STATISTICS,
        },
      );
      const raw = response.data;
      if (!raw || !Array.isArray(raw.data)) {
        throw new Error('History(since) request failed: unexpected response shape');
      }
      return { ...raw, data: dropErasedRecords(raw.data) };
    } catch (err) {
      // The interceptor already maps a storage-migration 503 to StorageMigratingError; any
      // other 503 on this endpoint means the archive is unavailable → same UI state.
      if (err instanceof StorageMigratingError) throw err;
      if (err instanceof ApiRequestError && err.status === 503) {
        const body = err.data as { progress?: number } | undefined;
        throw new StorageMigratingError(body?.progress, err.retryAfterSec, {
          headers: err.headers, data: err.data, method: err.method, url: err.url, cause: err,
        });
      }
      throw err;
    }
  }

  /** type=all combined history → the three families in one response (envelope #1). */
  async getHistoryAll(
    params: { unit_id: string; period: HistoryRequestParams['period']; from?: number; to?: number; limit?: number },
    options?: APIRequestOptions,
  ): Promise<HistoryAll> {
    const response = await this.client.get<{ data?: Partial<HistoryAll> }>(
      API_ENDPOINTS.HISTORY,
      {
        ...options,
        params: {
          unit_id: params.unit_id, period: params.period, type: 'all',
          limit: params.limit ?? 200,
          ...(params.from !== undefined ? { from: params.from } : {}),
          ...(params.to !== undefined ? { to: params.to } : {}),
        },
        timeout: HTTP_CONFIG.TIMEOUTS.STATISTICS,
      },
    );
    const data = response.data?.data ?? {};
    return {
      electrical: dropErasedRecords(data.electrical ?? []),
      energy: dropErasedRecords(data.energy ?? []),
      economics: dropErasedRecords(data.economics ?? []),
    };
  }

  // ===== Utility methods =====

  /**
   * Health check
   */
  async healthCheck(): Promise<boolean> {
    try {
      await this.client.get('/health', { timeout: 5000 });
      return true;
    } catch {
      return false;
    }
  }

  /**
   * One page of the hub's event log (`GET /api/v1/system/events`, root canon 10723b74).
   * Without `since` — the tail of this boot's RAM ring; with it — lines after `since`, files first.
   */
  async getHubEventsPage(q: { level: 'all' | 'event' | 'fail'; since?: number; limit?: number }, options?: APIRequestOptions): Promise<HubEventsPage> {
    if (isDemo()) return demoHubEvents(q.level);
    const params: Record<string, string | number> = { level: q.level, limit: q.limit ?? 1000 };
    if (q.since != null) params.since = q.since;
    const r = await this.client.get<HubEventsPage>('/api/v1/system/events', { ...options, params });
    return r.data;
  }

  /**
   * The hub's log lines with seq in (from, to], paging by `next` (at most `maxPages` requests —
   * the hub is slow, the log holds weeks). Oldest first.
   */
  async getHubEventsRange(level: 'all' | 'event' | 'fail', from: number, to: number, maxPages = 6): Promise<{ events: HubEvent[]; page: HubEventsPage | null }> {
    const out: HubEvent[] = [];
    let since = from;
    let page: HubEventsPage | null = null;
    for (let i = 0; i < maxPages; i++) {
      page = await this.getHubEventsPage({ level, since });
      for (const e of page.events) if (e.seq > from && e.seq <= to) out.push(e);
      if (!page.more || page.next <= since || page.next >= to) break;
      since = page.next;
    }
    return { events: out, page };
  }

  /** `GET /api/system/version` — firmware version, partition and `image_md5` (canon id). */
  async getHubVersion(options?: APIRequestOptions): Promise<HubVersion> {
    if (isDemo()) return demoHubVersion();
    const r = await this.client.get<HubVersion>('/api/system/version', options);
    return r.data;
  }

  /**
   * Get API version
   */
  async getApiVersion(): Promise<string> {
    const response = await this.client.get<{ version: string }>('/version');
    return response.data.version;
  }

  /**
   * Update base URL
   */
  updateBaseURL(baseURL: string): void {
    this.client.defaults.baseURL = baseURL;
  }

  /**
   * Get axios instance (for advanced usage)
   */
  getAxiosInstance(): AxiosInstance {
    return this.client;
  }
}

// ===== Singleton instance =====

let apiClientInstance: RBGridApiClient | null = null;

/**
 * Get or create API client instance
 */
export function getApiClient(config?: ApiClientConfig): RBGridApiClient {
  if (!apiClientInstance) {
    const baseURL = config?.baseURL || ENVIRONMENT.API_BASE_URL;
    apiClientInstance = new RBGridApiClient({ ...config, baseURL });
  }
  return apiClientInstance;
}

/**
 * Reset API client instance
 */
export function resetApiClient(): void {
  apiClientInstance = null;
}

// Export default instance
export default getApiClient();