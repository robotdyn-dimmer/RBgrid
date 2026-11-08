/**
 * Connection status for the shell (UX audit T2: HEU-01/03, HOME-01/19).
 *  - LinkStatus: «● на связи · обновлено 3 с назад» / «● нет связи · данные от 14:02» /
 *    «● подключение…» — in the rail footer and the mobile header, so the reader always knows
 *    whether the numbers are live.
 *  - DeviceUnreachable: replaces the page when the hub never answered — what to check and the
 *    ways out (retry / another device / demo) instead of an endless «Подключение…» spinner.
 */
import React, { useEffect, useState } from 'react';
import { Box, Typography, Button, Stack } from '@mui/material';
import { visuallyHidden } from '@mui/utils';
import WifiOffIcon from '@mui/icons-material/WifiOff';
import LockIcon from '@mui/icons-material/Lock';
import { Link as RouterLink } from 'react-router-dom';
import { Card } from '@rbgrid/design-system';
import { TelemetryConnState } from '../hooks/useTelemetryEntities';
import { connectDemo, disconnect, getBaseUrl, connectDevice, connectBroker, getBrokerUrl, getDeviceMac, getTransportKind } from '../services/connection';
import { useDsTokens } from '../theme/useDsTokens';
import { t, intlLocale, dateLocale } from '../i18n';
import { useDeviceClock, formatDeviceTime } from '../core/deviceTime';

export type LinkState = 'live' | 'stale' | 'connecting';

export function linkStateOf(conn: TelemetryConnState, hasData: boolean): LinkState {
  if (conn === 'error') return 'stale';
  return hasData ? 'live' : 'connecting';
}

/** Re-render every second (for «N с назад»). */
function useNow(): number {
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    const id = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(id);
  }, []);
  return now;
}

function agoText(ms: number): string {
  const s = Math.max(0, Math.round(ms / 1000));
  if (s < 60) return t('shell.agoS', { s });
  const m = Math.round(s / 60);
  return m < 60 ? t('shell.agoM', { m }) : t('shell.agoH', { h: Math.round(m / 60) });
}

export const LinkStatus: React.FC<{ state: LinkState; lastUpdate: number | null; demo?: boolean; compact?: boolean }> = ({
  state, lastUpdate, demo, compact,
}) => {
  const ds = useDsTokens();
  const now = useNow();
  const clock = useDeviceClock(); // «data from 18:04» on the hub's clock, as everywhere (L3P-12)
  const color = state === 'live' ? ds.statusOnline : state === 'stale' ? ds.sevCritical : ds.sevWarning;
  let text: string;
  if (demo) text = t('shell.linkDemo');
  else if (state === 'live') text = lastUpdate ? t('shell.linkLive', { ago: agoText(now - lastUpdate) }) : t('shell.linkLiveShort');
  else if (state === 'stale') {
    text = lastUpdate
      ? t('shell.linkStale', { time: formatDeviceTime(clock, Math.floor(lastUpdate / 1000), { hour: '2-digit', minute: '2-digit' }, dateLocale()) })
      : t('shell.linkNone');
  } else text = t('shell.linkConnecting');
  return (
    <Stack direction="row" spacing={0.75} alignItems="center" sx={{ minWidth: 0 }}>
      <Box sx={{ width: 8, height: 8, borderRadius: '50%', bgcolor: demo ? ds.inkMuted : color, flexShrink: 0 }} />
      {/* The ticking «обновлено N с назад» is not announced; a screen reader hears only a CHANGE of
          state (HEU-R07: the whole line was aria-live and re-read every second). */}
      <Typography aria-hidden sx={{ fontSize: compact ? 12 : 13, color: state === 'stale' && !demo ? ds.sevCritical : ds.inkMuted }} noWrap>
        {text}
      </Typography>
      <Box component="span" role="status" aria-live="polite" sx={visuallyHidden}>
        {demo ? t('shell.linkDemo') : state === 'live' ? t('shell.linkLiveShort') : state === 'stale' ? t('shell.linkNone') : t('shell.linkConnecting')}
      </Box>
    </Stack>
  );
};

/** The hub answers but withholds data without a read token (REST 401, the socket refused after
 *  its 101) — a token is missing or wrong, not «no connection» (root ws-ticket-app-check, step 3). */
export const DeviceNeedsToken: React.FC = () => {
  const ds = useDsTokens();
  const addr = getBaseUrl().replace(/^https?:\/\//, '') || t('shell.thisDevice');
  return (
    <Box sx={{ display: 'flex', justifyContent: 'center', py: { xs: 4, md: 8 } }}>
      <Box sx={{ maxWidth: 520, width: '100%' }}>
        <Card>
          <Stack spacing={2}>
            <Stack direction="row" spacing={1.5} alignItems="center">
              <LockIcon sx={{ color: ds.sevWarning }} />
              <Typography variant="h6" fontWeight={700}>{t('shell.tokenNeededTitle', { addr })}</Typography>
            </Stack>
            <Typography color="text.secondary">{t('shell.tokenNeededText')}</Typography>
            <Stack direction={{ xs: 'column', sm: 'row' }} spacing={1.5}>
              <Button variant="contained" component={RouterLink} to="/config#security">{t('shell.tokenNeededAction')}</Button>
              <Button variant="outlined" onClick={disconnect}>{t('shell.switchDeviceLong')}</Button>
            </Stack>
          </Stack>
        </Card>
      </Box>
    </Box>
  );
};

export const DeviceUnreachable: React.FC = () => {
  const ds = useDsTokens();
  const addr = getBaseUrl().replace(/^https?:\/\//, '') || t('shell.thisDevice');
  return (
    <Box sx={{ display: 'flex', justifyContent: 'center', py: { xs: 4, md: 8 } }}>
      <Box sx={{ maxWidth: 520, width: '100%' }}>
        <Card>
          <Stack spacing={2}>
            <Stack direction="row" spacing={1.5} alignItems="center">
              <WifiOffIcon sx={{ color: ds.sevCritical }} />
              <Typography variant="h6" fontWeight={700}>{t('shell.unreachableTitle', { addr })}</Typography>
            </Stack>
            <Typography color="text.secondary">{t('shell.unreachableText')}</Typography>
            <Box component="ul" sx={{ m: 0, pl: 2.5, color: 'text.secondary', fontSize: 14 }}>
              <li>{t('shell.unreachableHint1')}</li>
              <li>{t('shell.unreachableHint2')}</li>
              <li>{t('shell.unreachableHint3')}</li>
            </Box>
            <Stack direction={{ xs: 'column', sm: 'row' }} spacing={1.5}>
              {/* Reconnect in place — no page reload with seconds of white screen (HEU-R08). */}
              <Button variant="contained" onClick={() => (getTransportKind() === 'mqtt' ? connectBroker(getBrokerUrl(), getDeviceMac()) : connectDevice(getBaseUrl()))}>{t('common.retry')}</Button>
              <Button variant="outlined" onClick={disconnect}>{t('shell.switchDeviceLong')}</Button>
              <Button onClick={connectDemo}>{t('connect.openDemo')}</Button>
            </Stack>
          </Stack>
        </Card>
      </Box>
    </Box>
  );
};
