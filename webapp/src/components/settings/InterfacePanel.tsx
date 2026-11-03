/**
 * InterfacePanel — the interface language and the number/date format («Настройки → Интерфейс»).
 * The language is the HUB's (system.language, operator 18.10: one device, one user): choosing it
 * here writes it to the hub (live, no reboot) and every browser that opens the hub follows. Not
 * set → the browser's language. The list = the languages with a pack in the repository; a hub
 * that still declares a closed enum greys out the codes it doesn't take. The format stays a
 * per-browser preference.
 */
import React, { useState } from 'react';
import { Alert, MenuItem, Stack, TextField, Typography } from '@mui/material';
import { PanelCard } from '@rbgrid/design-system';
import { FORMATS, setFormat, setLang, langIntl, t, langFromDevice, browserLang } from '../../i18n';
import { useFormat, useLang, useLanguages } from '../../i18n/I18nProvider';
import { useCapabilities, useConfigShadow, useDeviceSession } from '../../core';
import { isDemo } from '../../services/connection';

/** «1 234,5 · 16.10.2026, 14:05» in the given locale. */
export function formatSample(locale: string): string {
  const n = (1234.5).toLocaleString(locale, { minimumFractionDigits: 1, maximumFractionDigits: 1 });
  const d = new Date(Date.UTC(2026, 9, 16, 14, 5)).toLocaleString(locale, { timeZone: 'UTC', day: '2-digit', month: '2-digit', year: 'numeric', hour: '2-digit', minute: '2-digit' });
  return `${n} · ${d}`;
}

const UNSET = '';

export const InterfacePanel: React.FC = () => {
  const lang = useLang();
  const fmt = useFormat();
  const langs = useLanguages();
  const caps = useCapabilities();
  const session = useDeviceSession();
  const system = useConfigShadow('system');
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState(false);

  const demo = isDemo();
  const hubHasLang = demo || 'language' in system.reported;
  const hubCode = demo ? lang : langFromDevice(system.reported.language) ?? UNSET;
  // A hub on an older canon accepts only its enum (RU / EN); the rest are shown, not offered.
  const accepts = (code: string) => demo || caps.languageEnum.length === 0
    || caps.languageEnum.some((c) => c.toLowerCase() === code);
  const label = (code: string) => langs.find((l) => l.code === code)?.label ?? code;

  const pick = async (code: string) => {
    setErr(false);
    if (demo) { await setLang(code || 'en'); return; }
    setBusy(true);
    try {
      await session.applyLiveField('system', 'language', code ? code.toUpperCase() : '');
    } catch {
      setErr(true);
    } finally { setBusy(false); }
  };

  return (
    <PanelCard title={t('ui.title')}>
      <Stack spacing={2}>
        <TextField select size="small" label={t('ui.language')} value={hubCode} disabled={busy || !hubHasLang}
          onChange={(e) => { void pick(e.target.value); }}
          SelectProps={{ displayEmpty: true }} InputLabelProps={{ shrink: true }}
          helperText={!hubHasLang ? t('ui.languageNoHub') : t('ui.languageHub')}>
          {!demo && <MenuItem value={UNSET}>{t('ui.languageUnset', { lang: langs.some((l) => l.code === browserLang()) ? label(browserLang()) : label('en') })}</MenuItem>}
          {langs.map((l) => (
            // Each language in its own name — a reader who can't read the current one still finds theirs.
            <MenuItem key={l.code} value={l.code} lang={l.code} disabled={!accepts(l.code)}>
              {l.label}{accepts(l.code) ? '' : ` — ${t('ui.languageHubNo')}`}
            </MenuItem>
          ))}
          {hubCode && !langs.some((l) => l.code === hubCode) && <MenuItem value={hubCode}>{hubCode}</MenuItem>}
        </TextField>
        {err && <Alert severity="error">{t('ui.languageFailed')}</Alert>}
        <TextField select size="small" label={t('ui.format')} value={fmt} onChange={(e) => setFormat(e.target.value)}
          helperText={t('ui.formatHelp')}>
          {FORMATS.map((f) => (
            <MenuItem key={f} value={f}>
              {f === 'auto' ? `${t('ui.formatAuto')} — ${formatSample(langIntl())}` : `${f} — ${formatSample(f)}`}
            </MenuItem>
          ))}
        </TextField>
        <Typography variant="body2" color="text.secondary">{t('ui.note')}</Typography>
      </Stack>
    </PanelCard>
  );
};
