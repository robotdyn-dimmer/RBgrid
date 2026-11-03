/**
 * CorsPanel — browser access policy of the hub (GET/POST /api/system/security, T10; admin token,
 * live apply). dev_cors=true → `Access-Control-Allow-Origin: *` (any site in this browser on the
 * LAN can READ telemetry/history; writes still need the admin token); false → only the listed
 * origins (≤ schema.limits.origins_max) are answered. Root (firmware note cors-option, canon 17ce1a71): the same policy now also
 * answers Chrome's Private-Network preflight. The default will flip to CLOSED in one canon once the
 * official cloud-app origin is named; this panel is the webapp half of that package.
 *
 * Self-lockout guard: closing CORS while THIS page's origin isn't listed would cut this very
 * window off the hub — so the current origin is offered and required before saving.
 */
import React, { useEffect, useState } from 'react';
import {
  Alert, Box, Button, Chip, Dialog, DialogActions, DialogContent, DialogTitle, Stack, Typography,
} from '@mui/material';
import { PanelCard, FormField, StatusChip } from '@rbgrid/design-system';
import { useCapabilities, useDeviceSession } from '../../core';
import type { SecurityConfig } from '../../core';
import { t } from '../../i18n';

const ORIGIN_RE = /^https?:\/\/[a-z0-9.-]+(:\d{1,5})?$/i;

export const CorsPanel: React.FC = () => {
  const session = useDeviceSession();
  const [cfg, setCfg] = useState<SecurityConfig | null>(null);
  const [draft, setDraft] = useState<SecurityConfig | null>(null);
  const [input, setInput] = useState('');
  const [confirmOpen, setConfirmOpen] = useState(false);
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState<{ sev: 'success' | 'error' | 'warning'; text: string } | null>(null);
  const here = window.location.origin;
  const MAX_ORIGINS = useCapabilities().limits.originsMax; // schema.limits.origins_max

  useEffect(() => {
    let on = true;
    session.transport.getSecurity().then((c) => { if (on) { setCfg(c); setDraft(c); } }).catch(() => undefined);
    return () => { on = false; };
  }, [session]);

  if (!draft) {
    return (
      <PanelCard title={t('cors.title')}>
        <Typography variant="body2" color="text.secondary">
          {t('cors.unavailable')}
        </Typography>
      </PanelCard>
    );
  }

  const dirty = JSON.stringify(draft) !== JSON.stringify(cfg);
  const lockout = !draft.dev_cors && !draft.origins.includes(here);
  const canAdd = ORIGIN_RE.test(input.trim()) && !draft.origins.includes(input.trim()) && draft.origins.length < MAX_ORIGINS;

  const save = async (next: SecurityConfig) => {
    setBusy(true);
    setMsg(null);
    try {
      await session.transport.setSecurity(next);
      setCfg(next);
      setDraft(next);
      setMsg({ sev: 'success', text: t('cors.saved') });
    } catch (e) {
      const err = e as { status?: number; response?: { status?: number } };
      const status = err?.status ?? err?.response?.status;
      setMsg({ sev: 'error', text: status === 401 || status === 403 ? t('common.adminTokenRequired') : t('cors.saveFailed') });
    } finally {
      setBusy(false);
    }
  };

  return (
    <PanelCard title={t('cors.title')}>
      <Stack spacing={1.5}>
        <Stack direction="row" spacing={1} alignItems="center" flexWrap="wrap" useFlexGap>
          <StatusChip
            status={draft.dev_cors ? 'dormant' : 'live'}
            label={draft.dev_cors ? t('cors.statusOpen') : t('cors.statusAllowlist')}
          />
        </Stack>
        <Typography variant="body2" color="text.secondary">
          {draft.dev_cors
            ? t('cors.openHint')
            : t('cors.allowlistHint')}
        </Typography>

        <Box>
          <Typography variant="overline" color="text.secondary">{t('cors.allowedOrigins', { n: draft.origins.length, max: MAX_ORIGINS })}</Typography>
          <Stack direction="row" spacing={1} flexWrap="wrap" useFlexGap sx={{ mt: 0.5 }}>
            {draft.origins.length === 0 && <Typography variant="body2" color="text.secondary">{t('cors.empty')}</Typography>}
            {draft.origins.map((o) => (
              <Chip key={o} label={o} size="small" onDelete={() => setDraft({ ...draft, origins: draft.origins.filter((x) => x !== o) })} />
            ))}
          </Stack>
          <Stack direction={{ xs: 'column', sm: 'row' }} spacing={1} sx={{ mt: 1 }} alignItems={{ sm: 'center' }}>
            <Box sx={{ flex: 1 }}>
              <FormField label={t('cors.originLabel')} value={input} onChange={setInput} placeholder="https://app.example.com" />
            </Box>
            <Button
              disabled={!canAdd}
              onClick={() => { setDraft({ ...draft, origins: [...draft.origins, input.trim()] }); setInput(''); }}
            >
              {t('cors.add')}
            </Button>
            {!draft.origins.includes(here) && draft.origins.length < MAX_ORIGINS && (
              <Button onClick={() => setDraft({ ...draft, origins: [...draft.origins, here] })}>
                {t('cors.addThis')}
              </Button>
            )}
          </Stack>
        </Box>

        {lockout && (
          <Alert severity="warning">
            {t('cors.lockout', { origin: here })}
          </Alert>
        )}
        {msg && <Alert severity={msg.sev}>{msg.text}</Alert>}

        <Stack direction="row" spacing={1} flexWrap="wrap" useFlexGap>
          {draft.dev_cors ? (
            <Button variant="outlined" onClick={() => setDraft({ ...draft, dev_cors: false })}>
              {t('cors.close')}
            </Button>
          ) : (
            <Button variant="outlined" color="warning" onClick={() => setConfirmOpen(true)}>
              {t('cors.openAll')}
            </Button>
          )}
          <Button variant="contained" disabled={!dirty || busy || lockout} onClick={() => save(draft)}>
            {busy ? t('common.saving') : t('common.save')}
          </Button>
        </Stack>
      </Stack>

      <Dialog open={confirmOpen} onClose={() => setConfirmOpen(false)} maxWidth="xs" fullWidth>
        <DialogTitle>{t('cors.confirmTitle')}</DialogTitle>
        <DialogContent>
          <Typography>
            {t('cors.confirmText')}
          </Typography>
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setConfirmOpen(false)}>{t('cors.cancel')}</Button>
          <Button color="warning" variant="contained" onClick={() => { setDraft({ ...draft, dev_cors: true }); setConfirmOpen(false); }}>
            {t('cors.confirmOk')}
          </Button>
        </DialogActions>
      </Dialog>
    </PanelCard>
  );
};
