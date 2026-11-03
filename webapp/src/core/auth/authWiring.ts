/**
 * Bearer-auth wiring for the Direct (HTTP) transport. Registers a request interceptor
 * on the shared api client that attaches `Authorization: Bearer <token>` to every
 * request when a token is set (the device ignores it on open routes, requires it on
 * the auth-guarded writes). Idempotent — safe to call on every provider mount.
 */
import { AxiosRequestConfig } from 'axios';
import { getApiClient } from '../../services/api';
import { TokenStore, createBrowserTokenStore } from './TokenStore';

/** App-wide token custody (localStorage-backed). */
export const tokenStore: TokenStore = createBrowserTokenStore();

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
