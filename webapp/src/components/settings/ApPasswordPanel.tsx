/**
 * ApPasswordPanel — the hub's own Wi-Fi access point (`rbgrid-<mac4>`, root 20.10 bf4621e4).
 * Only the user sets its password (operator): the app never generates or shows one. While the
 * first-login password is still in use (`user_set: false`) a soft reminder says so.
 * GET /api/system/ap {ssid, user_set} (admin) · POST /api/system/ap {password} — 8–63 printable
 * ASCII, "" = back to the first-login password; applies at once, AP clients reconnect.
 */
import React, { useEffect, useState } from 'react';
import { Stack, Button, Alert, Typography, TextField } from '@mui/material';
import { PanelCard, StatusChip } from '@rbgrid/design-system';
import { useDeviceSession } from '../../core';
import type { ApStatus } from '../../core/transport/types';
import { t } from '../../i18n';
import { useConfirm } from '../common/ConfirmDialog';
import { getApiClient } from '../../services/api';
import { RebootNowButton } from '../common/RebootNowButton';

/** 8–63 printable ASCII (WPA2 passphrase), as the hub checks it. */
export const apPasswordOk = (p: string): boolean => /^[\x20-\x7E]{8,63}$/.test(p);

export const ApPasswordPanel: React.FC = () => {
  const session = useDeviceSession();
  const confirm = useConfirm();
  const [ap, setAp] = useState<ApStatus | null>(null);
  const [loaded, setLoaded] = useState(false);
  const [value, setValue] = useState('');
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState<{ sev: 'success' | 'warning' | 'error'; text: string; reboot?: boolean } | null>(null);

  const refresh = () => { session.transport.getAp().then(setAp).catch(() => setAp(null)).finally(() => setLoaded(true)); };
  // Without the admin token /api/system/ap is 403 — the network name still comes from /wifi/status (root .139).
  const [wifiSsid, setWifiSsid] = useState<string | null>(null);
  useEffect(() => {
    refresh();
    getApiClient().getWifiStatus().then((s) => setWifiSsid(s.ap_ssid || null)).catch(() => undefined);
    /* eslint-disable-next-line react-hooks/exhaustive-deps */
  }, []);

  const write = async (password: string) => {
    setBusy(true); setMsg(null);
    try {
      const r = await session.transport.setApPassword(password);
      setValue('');
      const done = password ? t('ap.saved') : t('ap.resetDone');
      setMsg(r.applied === false ? { sev: 'warning', text: `${done} ${t('ap.afterRestart')}`, reboot: true } : { sev: 'success', text: done });
      refresh();
    } catch (e) {
      const status = (e as { response?: { status?: number } }).response?.status;
      setMsg({ sev: 'error', text: status === 401 || status === 403 ? t('ap.needToken') : status === 400 ? t('ap.rejected') : t('ap.saveError') });
    } finally { setBusy(false); }
  };

  const save = async () => {
    if (!apPasswordOk(value)) return;
    if (!(await confirm({ title: t('ap.confirmTitle'), body: t('ap.confirmBody') }))) return;
    await write(value);
  };
  const reset = async () => {
    if (!(await confirm({ title: t('ap.resetTitle'), body: t('ap.confirmBody'), danger: true }))) return;
    await write('');
  };

  const bad = value !== '' && !apPasswordOk(value);
  return (
    <PanelCard title={t('ap.title')}>
      <Stack direction="row" alignItems="center" spacing={1.5} mb={1.5} flexWrap="wrap" useFlexGap>
        <Typography variant="body2" color="text.secondary">{t('ap.network', { ssid: ap?.ssid || wifiSsid || 'rbgrid-…' })}</Typography>
        {ap && <StatusChip status={ap.userSet ? 'live' : 'dormant'} label={ap.userSet ? t('ap.userSet') : t('ap.firstLogin')} />}
      </Stack>
      {ap && !ap.userSet && <Alert severity="info" sx={{ mb: 2 }}>{t('ap.remind')}</Alert>}
      {/* The hub shows the status to the admin only (GET 401 without the token, .110 bf4621e4). */}
      {loaded && !ap && <Typography variant="body2" color="text.secondary" sx={{ mb: 2 }}>{t('ap.statusNeedsToken')}</Typography>}
      <Stack spacing={2}>
        <TextField
          size="small" fullWidth label={t('ap.newLabel')} type="password" value={value} autoComplete="new-password"
          onChange={(e) => setValue(e.target.value)}
          error={bad} helperText={bad ? t('ap.bad') : t('ap.help')}
        />
        <Stack direction="row" spacing={1} flexWrap="wrap" useFlexGap>
          <Button variant="contained" disabled={busy || !apPasswordOk(value)} onClick={save}>{t('ap.save')}</Button>
          {ap?.userSet && <Button color="inherit" disabled={busy} onClick={reset}>{t('ap.reset')}</Button>}
        </Stack>
        {msg && <Alert severity={msg.sev} action={msg.reboot ? <RebootNowButton /> : undefined}>{msg.text}</Alert>}
      </Stack>
    </PanelCard>
  );
};
