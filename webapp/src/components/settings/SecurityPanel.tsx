/**
 * SecurityPanel — API token management. The device is fail-open until a token is set;
 * once set, auth-guarded writes require Bearer (the TokenStore + transport handle that).
 * GET /api/system/token {token_set} · POST /api/system/token {token} (set / rotate / clear).
 */
import React, { useEffect, useState } from 'react';
import { Stack, Button, Alert, Typography, Box } from '@mui/material';
import { PanelCard, FormField, StatusChip } from '@rbgrid/design-system';
import { useDeviceSession } from '../../core';
import { t } from '../../i18n';
import { useConfirm } from '../common/ConfirmDialog';

export const SecurityPanel: React.FC = () => {
  const session = useDeviceSession();
  const [tokenSet, setTokenSet] = useState<boolean | null>(null);
  const [value, setValue] = useState('');
  const [busy, setBusy] = useState(false);
  const confirm = useConfirm();
  const [msg, setMsg] = useState<{ sev: 'success' | 'error'; text: string } | null>(null);

  const refresh = () => { session.transport.getTokenStatus().then(setTokenSet).catch(() => setTokenSet(null)); };
  useEffect(() => { refresh(); /* eslint-disable-next-line react-hooks/exhaustive-deps */ }, []);

  const save = async () => {
    if (!value.trim()) return;
    if (tokenSet && !(await confirm({ title: t('security.rotateConfirm') }))) return;
    setBusy(true); setMsg(null);
    try {
      await session.transport.setToken(value.trim());
      setValue('');
      setMsg({ sev: 'success', text: tokenSet ? t('security.rotated') : t('security.installed') });
      refresh();
    } catch {
      setMsg({ sev: 'error', text: t('security.saveError') });
    } finally { setBusy(false); }
  };

  const clear = async () => {
    if (!(await confirm({ title: t('security.clearConfirm'), danger: true, confirmLabel: t('common.delete') }))) return;
    setBusy(true); setMsg(null);
    try {
      await session.transport.setToken('');
      setMsg({ sev: 'success', text: t('security.cleared') });
      refresh();
    } catch {
      setMsg({ sev: 'error', text: t('security.clearFailed') });
    } finally { setBusy(false); }
  };

  return (
    <PanelCard title={t('security.title')}>
      <Stack direction="row" alignItems="center" spacing={1.5} mb={1.5}>
        <Typography variant="body2" color="text.secondary">{t('security.apiToken')}</Typography>
        {tokenSet === null
          ? <StatusChip status="dormant" label={t('security.unknown')} />
          : <StatusChip status={tokenSet ? 'live' : 'dormant'} label={tokenSet ? t('security.set') : t('security.notSet')} />}
      </Stack>
      <Stack spacing={2}>
        <FormField
          label={tokenSet ? t('security.newTokenLabel') : t('security.setTokenLabel')}
          type="password" value={value} onChange={setValue}
          placeholder={t('security.placeholder')}
        />
        <Box sx={{ display: 'flex', gap: 1 }}>
          <Button variant="contained" disabled={busy || !value.trim()} onClick={save}>
            {tokenSet ? t('security.rotate') : t('security.install')}
          </Button>
          {tokenSet && <Button color="error" disabled={busy} onClick={clear}>{t('common.delete')}</Button>}
        </Box>
      </Stack>
      {msg && <Alert severity={msg.sev} sx={{ mt: 1.5 }}>{msg.text}</Alert>}
    </PanelCard>
  );
};
