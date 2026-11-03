/**
 * MqttBrokerForm — the MQTT broker the hub connects to (Home Assistant listens there): host /
 * port / TLS / login / password → mqtt-config {broker_uri}. Applies after a hub restart (the hub
 * says requires_reboot) → «Перезагрузить сейчас». The hub masks stored credentials, so a stored
 * login / password must be typed again to keep them (the masked URI is refused).
 */
import React, { useEffect, useMemo, useState } from 'react';
import { Alert, Box, Button, FormControlLabel, Stack, Switch, TextField, Typography } from '@mui/material';
import { useConfigShadow, useDeviceSession } from '../../core';
import { t } from '../../i18n';
import { RebootNowButton } from '../common/RebootNowButton';
import { ApiRequestError } from '../../services/api';
import { BrokerForm, parseBrokerUri, buildBrokerUri } from './mqttUri';
import { RELEASE_HIDDEN } from '../../constants/release';

export const MqttBrokerForm: React.FC = () => {
  const session = useDeviceSession();
  const mqtt = useConfigShadow('mqtt');
  const rep = mqtt.reported as Record<string, unknown>;
  // Nothing saved → the hub runs its default broker (effective_uri): show that one.
  const stored = typeof rep.broker_uri === 'string' && rep.broker_uri ? rep.broker_uri
    : typeof rep.effective_uri === 'string' ? rep.effective_uri : '';
  const initial = useMemo(() => parseBrokerUri(stored), [stored]);
  const [f, setF] = useState<BrokerForm>(initial);
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState<{ sev: 'success' | 'warning' | 'error'; text: string; reboot?: boolean } | null>(null);
  useEffect(() => { setF(initial); }, [initial]);

  const canWrite = typeof session.transport.setMqttBroker === 'function';
  const uri = buildBrokerUri(f);
  const dirty = !!uri && uri !== stored;
  // TLS hidden in release builds (the hub can't verify a broker certificate yet); a saved mqtts://
  // URI is shown as is and kept — the switch reads it but can't be flipped.
  const tlsLocked = RELEASE_HIDDEN.mqttTls;
  const showTls = !tlsLocked || initial.tls;
  const set = (patch: Partial<BrokerForm>) => setF((x) => ({ ...x, ...patch }));

  const save = async () => {
    if (!uri || !session.transport.setMqttBroker) return;
    setBusy(true); setMsg(null);
    try {
      const r = await session.transport.setMqttBroker(uri);
      await mqtt.load().catch(() => undefined);
      setMsg(r.requiresReboot ? { sev: 'warning', text: t('mqttBroker.savedReboot'), reboot: true } : { sev: 'success', text: t('mqttBroker.saved') });
    } catch (e) {
      const auth = e instanceof ApiRequestError && (e.status === 401 || e.status === 403);
      setMsg({ sev: 'error', text: auth ? t('common.adminTokenRequired') : t('mqttBroker.failed', { reason: e instanceof Error ? e.message : String(e) }) });
    } finally { setBusy(false); }
  };

  return (
    <Box>
      <Typography variant="subtitle2" fontWeight={700} sx={{ mb: 1 }}>{t('mqttBroker.title')}</Typography>
      <Stack spacing={1.5}>
        <Box sx={{ display: 'grid', gap: 1.5, gridTemplateColumns: { xs: 'minmax(0, 1fr)', sm: 'minmax(0, 3fr) minmax(0, 1fr)' } }}>
          <TextField size="small" label={t('mqttBroker.host')} placeholder="192.168.1.100" value={f.host} disabled={!canWrite}
            onChange={(e) => set({ host: e.target.value })} />
          <TextField size="small" label={t('mqttBroker.port')} value={f.port} disabled={!canWrite} inputProps={{ inputMode: 'numeric' }}
            onChange={(e) => set({ port: e.target.value.replace(/\D/g, '') })} />
        </Box>
        {showTls && (
          <Box>
            <FormControlLabel disabled={!canWrite || tlsLocked} label={t('mqttBroker.tls')}
              control={<Switch checked={f.tls} onChange={(e) => set({ tls: e.target.checked, port: f.port === (f.tls ? '8883' : '1883') ? (e.target.checked ? '8883' : '1883') : f.port })} />} />
            {tlsLocked && <Typography variant="caption" color="text.secondary" component="div">{t('mqttBroker.tlsKept')}</Typography>}
          </Box>
        )}
        <Box sx={{ display: 'grid', gap: 1.5, gridTemplateColumns: { xs: 'minmax(0, 1fr)', sm: 'minmax(0, 1fr) minmax(0, 1fr)' } }}>
          <TextField size="small" label={t('mqttBroker.user')} value={f.user} disabled={!canWrite} autoComplete="off"
            onChange={(e) => set({ user: e.target.value })} />
          <TextField size="small" type="password" label={t('mqttBroker.pass')} value={f.pass} disabled={!canWrite} autoComplete="new-password"
            onChange={(e) => set({ pass: e.target.value })} />
        </Box>
        {f.maskedCreds && <Typography variant="caption" color="warning.main">{t('mqttBroker.maskedHint')}</Typography>}
        <Typography variant="caption" color="text.secondary">{t('mqttBroker.restartHint')}</Typography>
        <Stack direction="row" spacing={1} alignItems="center">
          <Button size="small" variant="contained" disabled={!canWrite || !dirty || busy} onClick={save}>
            {busy ? t('common.saving') : t('common.save')}
          </Button>
          {!uri && f.host && <Typography variant="caption" color="error.main">{t(f.pass && !f.user ? 'mqttBroker.passNoUser' : 'mqttBroker.invalid')}</Typography>}
        </Stack>
        {msg && <Alert severity={msg.sev} action={msg.reboot ? <RebootNowButton /> : undefined}>{msg.text}</Alert>}
      </Stack>
    </Box>
  );
};
