/**
 * BackupPanel — the hub's ONE restore point (on the device, /backup; not a file to download —
 * root RC-2): settings, points, tariffs, time, modules; tokens are not in it.
 * «Сохранить точку восстановления» overwrites the slot; «Восстановить и перезагрузить» puts it
 * back — the hub does not restart by itself after a restore, so the app sends the reboot.
 */
import React, { useCallback, useEffect, useState } from 'react';
import { Alert, Button, Stack, Typography } from '@mui/material';
import SaveIcon from '@mui/icons-material/SaveOutlined';
import RestoreIcon from '@mui/icons-material/Restore';
import { PanelCard } from '@rbgrid/design-system';
import { useDeviceSession } from '../../core';
import type { BackupStatus } from '../../core/transport/types';
import { getApiClient } from '../../services/api';
import { t, dateLocale } from '../../i18n';
import { useConfirm } from '../common/ConfirmDialog';
import { RebootNowButton } from '../common/RebootNowButton';

const when = (ts: number) => new Date(ts * 1000).toLocaleString(dateLocale(), { day: 'numeric', month: 'short', year: 'numeric', hour: '2-digit', minute: '2-digit' });

export const BackupPanel: React.FC = () => {
  const session = useDeviceSession();
  const confirm = useConfirm();
  const [st, setSt] = useState<BackupStatus | null | undefined>(undefined);
  const [busy, setBusy] = useState<'save' | 'restore' | null>(null);
  const [msg, setMsg] = useState<{ sev: 'success' | 'error' | 'info' | 'warning'; text: string; reboot?: boolean } | null>(null);

  const load = useCallback(async () => setSt(await session.transport.getBackupStatus().catch(() => null)), [session]);
  useEffect(() => { void load(); }, [load]);

  // fw 1.1.7 (root restore-point-errors): save and restore are all-or-nothing, and say which way they failed.
  const failText = (e: unknown, op: 'save' | 'restore') => {
    const r = (e as { response?: { status?: number; data?: { error_code?: string; data?: { error_code?: string } } } })?.response;
    const s = r?.status;
    const code = r?.data?.error_code ?? r?.data?.data?.error_code;
    if (s === 401 || s === 403) return t('common.adminTokenRequired');
    if (op === 'save' && s === 500) return t('backup.saveFailed');
    if (op === 'restore' && (code === 'BACKUP_INCOMPLETE' || s === 409)) return t('backup.incomplete');
    if (op === 'restore' && code === 'RESTORE_FAILED') return t('backup.restoreFailed');
    if (op === 'restore' && s === 404) return t('backup.none');
    return t('backup.failed');
  };

  const save = async () => {
    if (st?.exists && !(await confirm({ title: t('backup.overwriteTitle'), body: t('backup.overwriteBody'), confirmLabel: t('backup.save') }))) return;
    setBusy('save'); setMsg(null);
    try {
      const r = await session.transport.createBackup();
      setMsg({ sev: 'success', text: t('backup.saved', { n: r.filesBackedUp }) });
      await load();
    } catch (e) {
      setMsg({ sev: 'error', text: failText(e, 'save') });
      await load(); // a failed save leaves the slot unusable — show what the hub has now
    } finally { setBusy(null); }
  };

  const restore = async () => {
    if (!(await confirm({ title: t('backup.restoreTitle'), body: t('backup.restoreBody'), confirmLabel: t('backup.restore'), danger: true }))) return;
    setBusy('restore'); setMsg(null);
    let restored = -1;
    try {
      const r = await session.transport.restoreBackup();
      if (r.status !== 'OK') throw new Error(r.status);
      restored = r.filesRestored;
      // The hub doesn't restart by itself after a restore (RC-2) — the restored files load at start.
      await getApiClient().rebootDevice();
      setMsg({ sev: 'info', text: t('backup.restored', { n: restored }) });
    } catch (e) {
      // Restored but the restart didn't go: say so — the restart button finishes it.
      setMsg(restored >= 0 ? { sev: 'warning', text: t('backup.restoredNoReboot', { n: restored }), reboot: true } : { sev: 'error', text: failText(e, 'restore') });
    } finally { setBusy(null); }
  };

  const status = st === undefined ? t('common.loading')
    : st === null ? t('backup.unknown')
      : !st.exists ? t('backup.none')
        : st.sealed === false ? t('backup.unsealed')
          : st.meta?.ts ? t('backup.at', { when: when(st.meta.ts), n: st.meta.files ?? st.files })
            : t('backup.atUnknown', { n: st.files });

  return (
    <PanelCard title={t('backup.title')}>
      <Stack spacing={1.5}>
        <Typography variant="body2" color="text.secondary">{t('backup.intro')}</Typography>
        <Typography variant="body2" fontWeight={600}>{status}</Typography>
        <Stack direction="row" spacing={1} flexWrap="wrap" useFlexGap>
          <Button variant="outlined" startIcon={<SaveIcon />} disabled={!!busy || st === null} onClick={save}>
            {busy === 'save' ? t('common.saving') : t('backup.save')}
          </Button>
          <Button variant="outlined" color="warning" startIcon={<RestoreIcon />} disabled={!!busy || !st?.exists || st.sealed === false} onClick={restore}>
            {busy === 'restore' ? t('backup.restoring') : t('backup.restore')}
          </Button>
        </Stack>
        {msg && <Alert severity={msg.sev} action={msg.reboot ? <RebootNowButton /> : undefined}>{msg.text}</Alert>}
      </Stack>
    </PanelCard>
  );
};
