/**
 * DeviceLanguage — the interface language IS the hub's (system-config `system.language`, an
 * ISO 639-1 code, operator 18.10: one device, one user). Not set → the browser's language when a
 * pack exists → English (i18n.applyHubLang). Changed in «Настройки → Интерфейс», which writes it
 * to the hub. Demo has no hub language: the browser's.
 *
 * When the app is served by the hub itself and the hub can store a pack (schema endpoint
 * /api/system/lang), the language pack lives on the hub: read from there, fetched from the
 * repository and stored there when missing (one alternative language at a time).
 */
import { useEffect } from 'react';
import { useCapabilities, useConfigShadow } from '../core';
import { isDemo, getBaseUrl, deviceSameOrigin } from '../services/connection';
import { tokenStore } from '../core/auth/authWiring';
import { applyHubLang, langFromDevice, setHubLangStore, LangPack } from './i18n';

const LANG_STORE = '/api/system/lang';

export const DeviceLanguage: React.FC = () => {
  const system = useConfigShadow('system');
  const caps = useCapabilities();
  const loaded = Object.keys(system.reported).length > 0;
  const device = isDemo() ? null : langFromDevice(system.reported.language);
  const hubStores = !isDemo() && deviceSameOrigin() && caps.endpoints.some((e) => e.path === LANG_STORE);

  useEffect(() => {
    if (!hubStores) { setHubLangStore(null); return; }
    const base = getBaseUrl();
    setHubLangStore({
      async get(code) {
        const r = await fetch(`${base}/lang/${code}.json`, { cache: 'no-cache' });
        return r.ok ? ((await r.json()) as LangPack) : null;
      },
      async put(pack) {
        await fetch(`${base}${LANG_STORE}?code=${pack.code}`, {
          method: 'POST', headers: { 'Content-Type': 'application/json', ...tokenStore.authHeader() }, body: JSON.stringify(pack),
        });
      },
    });
  }, [hubStores]);

  // The language is needed on every page, not only where «Система» is read.
  useEffect(() => {
    if (!loaded && !isDemo()) system.load().catch(() => undefined);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [loaded]);

  useEffect(() => {
    if (loaded || isDemo()) void applyHubLang(device);
  }, [device, loaded]);
  return null;
};
