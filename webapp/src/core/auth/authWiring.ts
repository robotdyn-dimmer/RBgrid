/**
 * Bearer-auth wiring for the Direct (HTTP) transport. Registers a request interceptor
 * on the shared api client that attaches `Authorization: Bearer <token>` to every
 * request when a token is set (the device ignores it on open routes, requires it on
 * the auth-guarded writes). Idempotent — safe to call on every provider mount.
 */
import { AxiosRequestConfig } from 'axios';
import { getApiClient } from '../../services/api';
import { TokenStore, createBrowserTokenStore, tokenScopeOf } from './TokenStore';
import { getBaseUrl } from '../../services/connection';

/** App-wide token custody (localStorage-backed), kept per hub origin (Kimi #1). */
export const tokenStore: TokenStore = createBrowserTokenStore(() =>
  tokenScopeOf(getBaseUrl(), typeof window !== 'undefined' ? window.location?.href : undefined));

let installed = false;

export function installBearerAuth(store: TokenStore = tokenStore): void {
  if (installed) return;
  installed = true;
  getApiClient().addRequestInterceptor((config: AxiosRequestConfig) => {
    const auth = store.authHeader();
    if (Object.keys(auth).length > 0) {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      config.headers = { ...(config.headers as any), ...auth } as any;
    }
    return config;
  });
}
