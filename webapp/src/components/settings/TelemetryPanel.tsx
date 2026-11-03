/**
 * TelemetryPanel — the one place for the hub's live data stream (operator 18.10: telemetry was
 * spread over «Система», «Телеметрия» and «Профиль телеметрии»). Top to bottom:
 *  1. Profile (lean / full) — the only writable part: POST telemetry-config {profile}, live.
 *  2. State: apps connected to the live stream now (telemetry-config ws_clients).
 *  3. Technical details, folded: the frame format.
 * telemetry-config's telemetry_enabled / channels / json_buffer_size / compact_json are NOT shown:
 * a stored copy nothing applies (root 18.10 — the module runs on main.cpp: WebSocket on, MQTT when
 * a broker is set, HTTP off). Making them real is a planned firmware change.
 * system-config's enable_telemetry / max_retry_attempts / retry_delay_ms are NOT here: the hub
 * stores them but nothing reads them (firmware rbtelemetry uses telemetry-config) — hidden.
 */
import React, { useEffect, useState } from 'react';
import { Alert, Box, Button, Collapse, Stack, Typography } from '@mui/material';
import { PanelCard, SettingRow } from '@rbgrid/design-system';
import { useCapabilities, useConfigShadow, useDeviceSession } from '../../core';
import { t } from '../../i18n';


export const TelemetryPanel: React.FC = () => {
  const caps = useCapabilities();
  const session = useDeviceSession();
  const tel = useConfigShadow('telemetry');
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState<{ ok: boolean; text: string } | null>(null);
  const [tech, setTech] = useState(false);

  useEffect(() => { tel.load().catch(() => undefined); /* eslint-disable-next-line react-hooks/exhaustive-deps */ }, []);

  const r = tel.reported;
  const profile = caps.telemetryProfile;
  const canSwitch = caps.flags.telemetryConfigPost;
  const clients = Array.isArray(r.ws_clients) ? r.ws_clients.length : null;

  const set = async (p: 'lean' | 'full') => {
    setBusy(true); setMsg(null);
    try {
      await session.transport.setTelemetryProfile(p);
      await session.init(); // capabilities carry the live profile
      await tel.load().catch(() => undefined);
      setMsg({ ok: true, text: t('telemetry.switched', { profile: t(p === 'lean' ? 'telemetry.leanLower' : 'telemetry.fullLower') }) });
    } catch {
      setMsg({ ok: false, text: t('telemetry.switchFailed') });
    } finally { setBusy(false); }
  };

  return (
    <PanelCard title={t('telemetry.title')}>
      <Stack spacing={1.5}>
        <Typography variant="body2" color="text.secondary">{t('telemetry.intro')}</Typography>

        <Typography variant="overline" color="text.secondary" sx={{ lineHeight: 1.6 }}>{t('telemetry.profile')}</Typography>
        <Stack direction="row" spacing={1}>
          {(['lean', 'full'] as const).map((p) => (
            <Button key={p} size="small" variant={profile === p ? 'contained' : 'outlined'} disabled={busy || !canSwitch} onClick={() => set(p)}>
              {t(p === 'lean' ? 'telemetry.lean' : 'telemetry.full')}
            </Button>
          ))}
        </Stack>
        <Typography variant="caption" color="text.secondary">
          {t(profile === 'lean' ? 'telemetry.leanHint' : 'telemetry.fullHint')} {t('telemetry.liveHint')}
        </Typography>
        {msg && <Alert severity={msg.ok ? 'success' : 'error'}>{msg.text}</Alert>}

        <Typography variant="overline" color="text.secondary" sx={{ lineHeight: 1.6, pt: 0.5 }}>{t('telemetry.state')}</Typography>
        {clients != null && <SettingRow label={t('telemetry.clients')} value={String(clients)} />}

        <Box>
          <Button size="small" onClick={() => setTech((v) => !v)}>{tech ? t('telemetry.techHide') : t('telemetry.techShow')}</Button>
          <Collapse in={tech}>
            <Stack spacing={1} sx={{ mt: 1 }}>
              {r.frame != null && <SettingRow label={t('telemetry.frame')} value={`v${String(r.frame)}`} />}
            </Stack>
          </Collapse>
        </Box>
        <Typography variant="caption" color="text.secondary">{t('telemetry.firmwareOwned')}</Typography>
      </Stack>
    </PanelCard>
  );
};
