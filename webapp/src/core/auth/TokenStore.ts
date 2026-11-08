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

/**
 * The token belongs to ONE hub (Kimi #1): with a `scope` (the target's origin) each hub keeps its
 * own slot, so switching hubs — or a `?device=` link to another host — never sends one hub's token
 * to another. Without a scope (tests, single-target use) there is one slot.
 */
export class TokenStore {
  private readonly mem = new Map<string, string | null>();
  private readonly storage?: KeyValueStorage;
  private readonly scope?: () => string;

  constructor(storage?: KeyValueStorage, initial?: string | null, scope?: () => string) {
    this.storage = storage;
    this.scope = scope;
    if (initial !== undefined) this.mem.set(this.key(), initial);
  }

  private key(): string {
    const sc = this.scope?.();
    return sc ? `${STORAGE_KEY}@${sc}` : STORAGE_KEY;
  }

  get(): string | null {
    const k = this.key();
    if (!this.mem.has(k)) this.mem.set(k, this.storage?.getItem(k) ?? null);
    return this.mem.get(k) ?? null;
  }

  has(): boolean {
    const t = this.get();
    return t != null && t.length > 0;
  }

  set(token: string): void {
    const k = this.key();
    this.mem.set(k, token);
    this.storage?.setItem(k, token);
  }

  clear(): void {
    const k = this.key();
    this.mem.set(k, null);
    this.storage?.removeItem(k);
  }

  /** Authorization header to merge into a request — `{}` when no token is set. */
  authHeader(): Record<string, string> {
    const t = this.get();
    return t ? { Authorization: `Bearer ${t}` } : {};
  }
}

/** The origin a token is kept for: the hub's base URL, or the page's own origin when served by the hub. */
export function tokenScopeOf(baseUrl: string, pageHref?: string): string {
  try { return new URL(baseUrl || '/', pageHref ?? 'http://localhost/').origin; } catch { return baseUrl; }
}

/**
 * One-time move of the pre-1.1.7 global token into the slot of the hub it was saved for (the
 * persisted connection, never a deep link). Without a persisted hub it is dropped — it can't be
 * told which hub it belongs to; the app asks for it again.
 */
export function migrateLegacyToken(ls: KeyValueStorage, pageHref?: string): void {
  const legacy = ls.getItem(STORAGE_KEY);
  if (legacy == null) return;
  try {
    const conn = JSON.parse(ls.getItem('rbgrid.connection') ?? 'null') as { baseUrl?: string; demo?: boolean } | null;
    if (conn && !conn.demo && typeof conn.baseUrl === 'string') {
      const k = `${STORAGE_KEY}@${tokenScopeOf(conn.baseUrl, pageHref)}`;
      if (ls.getItem(k) == null) ls.setItem(k, legacy);
    }
  } catch { /* unreadable connection → drop */ }
  ls.removeItem(STORAGE_KEY);
}

/** Default app instance backed by localStorage (guarded for non-browser/test contexts), one slot per hub. */
export function createBrowserTokenStore(scope?: () => string): TokenStore {
  const ls: KeyValueStorage | undefined =
    typeof window !== 'undefined' && window.localStorage ? window.localStorage : undefined;
  if (ls && scope) {
    try { migrateLegacyToken(ls, window.location?.href); } catch { /* storage blocked */ }
  }
  return new TokenStore(ls, undefined, scope);
}
