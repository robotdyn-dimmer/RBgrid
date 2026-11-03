/**
 * SiteNamePanel — the site's name and place, saved by its own button (wizard on a fresh hub,
 * 17.10 W191-01: the «site & time» step showed ~35 service fields of the system domain with no
 * save button; «Next» left them unsaved and «Done» said «configured»). Only these two fields go
 * out — a partial `{"system":{…}}`, absent fields keep their value.
 */
import React, { useEffect, useState } from 'react';
import { Alert, Button, Stack, TextField } from '@mui/material';
import { PanelCard } from '@rbgrid/design-system';
import { getApiClient } from '../../services/api';
import { t } from '../../i18n';
import { useDeviceSession } from '../../core';

/** The hub's factory placeholders read as empty — the user names the site, not «Default». */
const PLACEHOLDER = new Set(['Default', 'Unknown', '']);

export const SiteNamePanel: React.FC<{ onSaved?: () => void }> = ({ onSaved }) => {
  const session = useDeviceSession();
  const [name, setName] = useState('');
  const [place, setPlace] = useState('');
  const [orig, setOrig] = useState<{ name: string; place: string } | null>(null);
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState<{ ok: boolean; text: string } | null>(null);

  useEffect(() => {
    let on = true;
    getApiClient().getSystemSettings()
      .then((sys) => {
        const s = (sys ?? {}) as unknown as Record<string, unknown>;
        const n = typeof s.home_name === 'string' && !PLACEHOLDER.has(s.home_name) ? s.home_name : '';
        const p = typeof s.location === 'string' && !PLACEHOLDER.has(s.location) ? s.location : '';
        if (on) { setName(n); setPlace(p); setOrig({ name: n, place: p }); }
      })
      .catch(() => { if (on) setOrig({ name: '', place: '' }); });
    return () => { on = false; };
  }, []);

  const dirty = !!orig && (name.trim() !== orig.name || place.trim() !== orig.place);
  const save = async () => {
    setBusy(true); setMsg(null);
    try {
      await getApiClient().updateSystemConfig({ home_name: name.trim() || 'Default', location: place.trim() || 'Unknown' });
      setOrig({ name: name.trim(), place: place.trim() });
      // «Настройки → Система» edits the same fields through the shadow — re-read it there.
      session.loadDomain('system').catch(() => undefined);
      setMsg({ ok: true, text: t('siteName.saved') });
      onSaved?.();
    } catch {
      setMsg({ ok: false, text: t('siteName.failed') });
    } finally { setBusy(false); }
  };

  return (
    <PanelCard title={t('siteName.title')}>
      <Stack spacing={1.5}>
        <TextField size="small" label={t('siteName.name')} placeholder={t('siteName.namePh')} value={name} onChange={(e) => setName(e.target.value)} inputProps={{ maxLength: 31 }} />
        <TextField size="small" label={t('siteName.place')} placeholder={t('siteName.placePh')} value={place} onChange={(e) => setPlace(e.target.value)} inputProps={{ maxLength: 31 }} />
        <Stack direction="row" spacing={1} alignItems="center">
          <Button variant="contained" size="small" disabled={!dirty || busy} onClick={save}>{busy ? t('siteName.saving') : t('siteName.save')}</Button>
          {dirty && <Alert severity="warning" sx={{ py: 0 }}>{t('siteName.unsaved')}</Alert>}
        </Stack>
        {msg && <Alert severity={msg.ok ? 'success' : 'error'}>{msg.text}</Alert>}
      </Stack>
    </PanelCard>
  );
};
