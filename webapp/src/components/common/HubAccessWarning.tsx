/**
 * HubAccessWarning — the hub's own access point stays on (APSTA) with the public setup password until
 * the user changes it, and there is no admin token by default: anyone nearby can join and change the
 * hub. Operator 03.11 via root: keep the behaviour, warn. Full warning while neither is done, a lighter
 * note while one is; nothing once both are. Without a token the hub won't show the AP status (403), so
 * «no token» alone decides then. Hideable for this browser session; it comes back next time.
 */
import React, { useEffect, useState } from 'react';
import { Alert, Button, Stack } from '@mui/material';
import { Link as RouterLink } from 'react-router-dom';
import { useDeviceSession } from '../../core';
import { t } from '../../i18n';
import { getApiClient } from '../../services/api';
import { RELEASE_HIDDEN } from '../../constants/release';
import { isDemo } from '../../services/connection';
import { fwAtLeast } from '../../entities/hubVersion';
import { useHubVersion } from '../../hooks/useHubVersion';

export type AccessLevel = 'full' | 'token' | 'ap' | null;

/** tokenSet null = unknown; apUserSet null = the hub didn't say (no token → 403). */
export function accessLevel(tokenSet: boolean | null, apUserSet: boolean | null): AccessLevel {
  if (tokenSet === null) return null;
  if (!tokenSet) return apUserSet === true ? 'token' : 'full';
  return apUserSet === false ? 'ap' : null;
}

const HIDE_KEY = 'rbgrid.accessWarning.hidden';
const readHidden = (): boolean => { try { return sessionStorage.getItem(HIDE_KEY) === '1'; } catch { return false; } };

export const HubAccessWarning: React.FC<{ sx?: object }> = ({ sx }) => {
  const session = useDeviceSession();
  const [level, setLevel] = useState<AccessLevel>(null);
  const [ssid, setSsid] = useState('rbgrid-…');
  const [hidden, setHidden] = useState(readHidden);
  // Rides with the AP-password block (v1.1.2, hub ≥ 1.1.2) — the warning has to link somewhere real.
  const on112 = fwAtLeast(useHubVersion(), [1, 1, 2]) === true;
  const enabled = !RELEASE_HIDDEN.apPassword && !isDemo() && on112;

  useEffect(() => {
    if (!enabled) return;
    let on = true;
    (async () => {
      const token = await session.transport.getTokenStatus().catch(() => null);
      const ap = await session.transport.getAp().catch(() => null);
      if (!on) return;
      // No token → /ap is 403; the name still comes from /wifi/status (root .139).
      const wifi = ap?.ssid ? null : await getApiClient().getWifiStatus().catch(() => null);
      if (!on) return;
      const name = ap?.ssid || wifi?.ap_ssid;
      if (name) setSsid(name);
      setLevel(accessLevel(token, ap ? ap.userSet : null));
    })();
    return () => { on = false; };
  }, [session, enabled]);

  if (!enabled || !level || hidden) return null;
  const hide = () => { try { sessionStorage.setItem(HIDE_KEY, '1'); } catch { /* private mode */ } setHidden(true); };
  return (
    <Alert severity={level === 'full' ? 'warning' : 'info'} sx={{ mb: 2, ...sx }}
      action={
        <Stack direction="row" spacing={0.5} alignItems="center" flexWrap="wrap" useFlexGap>
          {level !== 'token' && <Button color="inherit" size="small" variant="outlined" component={RouterLink} to="/config#hub-ap" sx={{ whiteSpace: 'nowrap' }}>{t('access.toAp')}</Button>}
          {level !== 'ap' && <Button color="inherit" size="small" variant="outlined" component={RouterLink} to="/config#security" sx={{ whiteSpace: 'nowrap' }}>{t('access.toToken')}</Button>}
          <Button color="inherit" size="small" onClick={hide}>{t('access.hide')}</Button>
        </Stack>
      }>
      {t(`access.${level}`, { ssid })}
    </Alert>
  );
};
