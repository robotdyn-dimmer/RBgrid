/**
 * NodesPanel — the ESP-NOW remote-node topology (hub + wireless metering nodes).
 * Shows hub status + each node's liveness/health/units, and drives the pairing lifecycle
 * (pair / enable / disable / remove) as actions. pair/add register units on boot → reboot;
 * enable/disable/remove are immediate.
 */
import React, { useState } from 'react';
import { Box, Typography, Stack, Button, Alert, CircularProgress, Chip } from '@mui/material';
import WifiTetheringIcon from '@mui/icons-material/WifiTethering';
import { Card, StatusChip, InfoField, SettingRow } from '@rbgrid/design-system';
import { useEspnow, useDeviceSession, EspnowAction, ESPNOW_REBOOT_ACTIONS } from '../../core';
import { t } from '../../i18n';
import { RebootNowButton } from '../common/RebootNowButton';
import { useConfirm } from '../common/ConfirmDialog';

export const NodesPanel: React.FC = () => {
  const { data, loading, reloadNow } = useEspnow();
  const session = useDeviceSession();
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState<string | null>(null);
  const confirm = useConfirm();

  const act = async (action: EspnowAction, mac?: string) => {
    setBusy(true);
    setMsg(null);
    const job = session.jobs.start('espnow', { action, ...(mac ? { mac } : {}) });
    try {
      const res = await session.transport.espnowAction(action, {
        ...(mac ? { mac } : {}),
        ...(action === 'pair' ? { seconds: 60 } : {}),
      });
      if (ESPNOW_REBOOT_ACTIONS.has(action) || res.reboot_to_register) {
        session.jobs.awaitReboot(job.id);
        setMsg(t('nodes.acceptedReboot'));
      } else {
        session.jobs.resolve(job.id);
        setMsg(res.ok ? t('nodes.done') : (res.message ?? t('nodes.actionFailed')));
        await reloadNow();
      }
    } catch {
      session.jobs.fail(job.id, 'request failed');
      setMsg(t('nodes.actionFailedHint'));
    } finally {
      setBusy(false);
    }
  };

  return (
    <Box sx={{ mb: 3 }}>
      <Card>
        <Stack direction="row" alignItems="center" spacing={1.5} mb={1.5}>
          <Typography variant="h6" fontWeight={700}>{t('nodes.title')}</Typography>
          {data && <StatusChip status={data.running ? 'live' : 'dormant'} label={data.running ? t('nodes.channel', { channel: data.channel }) : t('nodes.stopped')} />}
          {data?.pairing_open && <Chip size="small" color="warning" label={t('nodes.pairing')} />}
          <Box sx={{ ml: 'auto' }}>
            <Button
              startIcon={<WifiTetheringIcon />} disabled={busy} onClick={() => act('pair')}
            >
              {t('nodes.pair')}
            </Button>
          </Box>
        </Stack>

        {msg && <Alert severity="info" sx={{ mb: 2 }} action={msg === t('nodes.acceptedReboot') ? <RebootNowButton /> : undefined}>{msg}</Alert>}
        {loading && <Box sx={{ display: 'flex', justifyContent: 'center', py: 2 }}><CircularProgress size={22} aria-label={t('common.loading')} /></Box>}
        {!loading && data && data.nodes.length === 0 && (
          <Typography color="text.secondary" variant="body2">{t('nodes.empty')}</Typography>
        )}

        <Stack spacing={1.5}>
          {data?.nodes.map((n) => (
            <Box key={n.mac} sx={{ border: 1, borderColor: 'divider', borderRadius: 2, p: 1.5, opacity: n.disabled ? 0.6 : 1 }}>
              <Stack direction="row" alignItems="center" spacing={1.5} mb={1}>
                <Typography variant="subtitle2" fontWeight={700}>{n.mac}</Typography>
                <StatusChip status={n.online ? 'live' : 'offline'} label={n.online ? t('nodes.online') : t('nodes.offline')} />
                {n.disabled && <Chip size="small" label={t('nodes.disabled')} variant="outlined" />}
                <Box sx={{ ml: 'auto' }}>
                  <Stack direction="row" spacing={1}>
                    <Button size="small" disabled={busy} onClick={() => act(n.disabled ? 'enable' : 'disable', n.mac)}>
                      {n.disabled ? t('nodes.enable') : t('nodes.disable')}
                    </Button>
                    <Button size="small" color="error" disabled={busy} onClick={async () => { if (await confirm({ title: t('nodes.removeTitle', { mac: n.mac }), body: t('nodes.removeBody'), confirmLabel: t('common.delete'), danger: true })) act('remove', n.mac); }}>{t('common.delete')}</Button>
                  </Stack>
                </Box>
              </Stack>
              <Stack direction="row" spacing={3} flexWrap="wrap" useFlexGap sx={{ mb: 1 }}>
                <InfoField label={t('nodes.lastSeen')} value={n.last_seen_s < 0 ? t('nodes.never') : t('nodes.seconds', { s: n.last_seen_s })} />
                <InfoField label={t('nodes.loss')} value={`${n.sync.loss_pct}%`} />
                <InfoField label={t('nodes.skew')} value={`${n.sync.skew_ppm} ppm`} />
                <InfoField label={t('nodes.jitter')} value={`${n.sync.resid_sd_us} µs`} />
              </Stack>
              {n.channels.map((c) => (
                <SettingRow key={c.ch} label={t('nodes.channelPoint', { ch: c.ch, unit: c.unit_id })} value={c.present ? t('nodes.online') : '—'} />
              ))}
            </Box>
          ))}
        </Stack>
      </Card>
    </Box>
  );
};
