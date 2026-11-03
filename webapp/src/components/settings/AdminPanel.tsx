/**
 * AdminPanel — device admin: reboot + factory reset.
 * The reset is v1.1.2's real one (root 01d14d6, operator 03.11): settings, points, tariff, Wi-Fi, tokens
 * and the AP password go, history too unless kept, and the hub restarts as on first boot — its own
 * access point + portal, so the app loses it (said before and after). Older hubs get no reset button:
 * their restore-system-config only undid the last site-settings save, it wasn't a factory reset.
 * Renders through the @rbgrid/design-system settings surface (PanelCard).
 */
import React, { useEffect, useRef, useState } from 'react';
import { Button, Stack, FormControlLabel, Checkbox, Typography } from '@mui/material';
import RestartAltIcon from '@mui/icons-material/RestartAlt';
import SettingsBackupRestoreIcon from '@mui/icons-material/SettingsBackupRestore';
import { PanelCard } from '@rbgrid/design-system';
import { getApiClient } from '../../services/api';
import { t } from '../../i18n';
import { useConfirm } from '../common/ConfirmDialog';
import { fwAtLeast } from '../../entities/hubVersion';
import { useHubVersion } from '../../hooks/useHubVersion';
import { RELEASE_HIDDEN } from '../../constants/release';
import { isDemo } from '../../services/connection';

/** The «also erase the history» box inside the confirm dialog; its value is read through a ref. */
const EraseHistory: React.FC<{ onChange: (v: boolean) => void }> = ({ onChange }) => {
  const [v, setV] = useState(true);
  return (
    <FormControlLabel sx={{ mt: 1 }} label={t('admin.resetEraseHistory')}
      control={<Checkbox checked={v} onChange={(e) => { setV(e.target.checked); onChange(e.target.checked); }} />} />
  );
};

export const AdminPanel: React.FC = () => {
  const [msg, setMsg] = useState<string | null>(null);
  const [busy, setBusy] = useState<'reboot' | 'restore' | null>(null);
  const confirm = useConfirm();
  const hubVersion = useHubVersion();
  const canReset = !RELEASE_HIDDEN.factoryReset && (isDemo() || fwAtLeast(hubVersion, [1, 1, 2]) === true);
  const [apSsid, setApSsid] = useState<string | null>(null);
  const erase = useRef(true);

  useEffect(() => {
    if (!canReset) return;
    getApiClient().getWifiStatus().then((s) => setApSsid(s.ap_ssid || null)).catch(() => undefined);
  }, [canReset]);

  const reboot = async () => {
    if (!(await confirm({ title: t('admin.rebootConfirm'), body: t('admin.rebootBody'), confirmLabel: t('admin.rebootAction'), danger: true }))) return;
    setBusy('reboot');
    setMsg(null);
    try {
      await getApiClient().rebootDevice();
      setMsg(t('admin.rebootSent'));
    } catch {
      setMsg(t('admin.rebootFailed'));
    } finally {
      setBusy(null);
    }
  };

  const reset = async () => {
    const ssid = apSsid ?? 'rbgrid-…';
    erase.current = true;
    const ok = await confirm({
      title: t('admin.resetTitle'),
      body: (
        <>
          <Typography variant="body2">{t('admin.resetBody', { ssid })}</Typography>
          <EraseHistory onChange={(v) => { erase.current = v; }} />
        </>
      ),
      confirmLabel: t('admin.resetAction'),
      danger: true,
    });
    if (!ok) return;
    setBusy('restore');
    setMsg(null);
    try {
      const r = await getApiClient().factoryReset(erase.current ? 'erase' : 'keep');
      setMsg(r.status === 'ok' ? t('admin.resetSent', { ssid }) : t('admin.resetRefused', { why: r.message ?? '' }));
    } catch {
      setMsg(t('admin.restoreFailed'));
    } finally {
      setBusy(null);
    }
  };

  return (
    <PanelCard title={t('admin.title')} alert={msg ?? undefined} alertSeverity="info">
      <Stack direction="row" spacing={2} flexWrap="wrap" useFlexGap>
        <Button variant="outlined" color="warning" startIcon={<RestartAltIcon />} disabled={!!busy} onClick={reboot}>
          {busy === 'reboot' ? t('admin.rebooting') : t('admin.reboot')}
        </Button>
        {canReset && (
          <Button variant="outlined" color="error" startIcon={<SettingsBackupRestoreIcon />} disabled={!!busy} onClick={reset}>
            {busy === 'restore' ? t('admin.restoring') : t('admin.restore')}
          </Button>
        )}
      </Stack>
    </PanelCard>
  );
};
