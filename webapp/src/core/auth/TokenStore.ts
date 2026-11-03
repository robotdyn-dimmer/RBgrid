/**
 * TokenStore — client custody of the device API token.
 *
 * The device gates writes with `Authorization: Bearer <token>` (fail-open until a
 * token is set; auth-guarded routes confirmed by root 2026-06-23: POST hardware/tariff/
 * telemetry/units/mqtt config, POST+DELETE /api/v1/units, reboot/restore/ota/time-sync,
 * POST system-config). With no app server, the browser holds the token — persisted to
 * storage so it survives reload (operator tool). NEVER logged.
 *
 * Storage is injectable so it's testable without a real `localStorage`.
 */
const STORAGE_KEY = 'rbgrid.token';

export interface KeyValueStorage {
  getItem(key: string): string | null;
  setItem(key: string, value: string): void;
  removeItem(key: string): void;
}

export class TokenStore {
  private token: string | null;
  private readonly storage?: KeyValueStorage;

  constructor(storage?: KeyValueStorage, initial?: string | null) {
    this.storage = storage;
    this.token = initial ?? storage?.getItem(STORAGE_KEY) ?? null;
  }

  get(): string | null {
    return this.token;
  }

  has(): boolean {
    return this.token != null && this.token.length > 0;
  }

  set(token: string): void {
    this.token = token;
    this.storage?.setItem(STORAGE_KEY, token);
  }

  clear(): void {
    this.token = null;
    this.storage?.removeItem(STORAGE_KEY);
  }

  /** Authorization header to merge into a request — `{}` when no token is set. */
  authHeader(): Record<string, string> {
    return this.has() ? { Authorization: `Bearer ${this.token}` } : {};
  }
}

/** Default app instance backed by localStorage (guarded for non-browser/test contexts). */
export function createBrowserTokenStore(): TokenStore {
  const ls: KeyValueStorage | undefined =
    typeof window !== 'undefined' && window.localStorage ? window.localStorage : undefined;
  return new TokenStore(ls);
}
