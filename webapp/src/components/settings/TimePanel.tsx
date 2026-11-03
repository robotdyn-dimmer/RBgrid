/**
 * TimePanel — the hub clock (replaces the read-only generic time editor). time-config has no
 * POST; writes go through the ACTION endpoint POST /api/system/time-sync (admin token):
 * save_config (timezone applies live) and sync_time (SNTP now, or this browser's clock for a
 * LAN without internet). Timezone is a POSIX TZ string on the device — presets avoid typing it.
 */
import React, { useEffect, useState } from 'react';
import { Alert, Box, Button, MenuItem, Stack, TextField, Typography } from '@mui/material';
import { PanelCard, FormField, StatusChip, ToggleRow } from '@rbgrid/design-system';
import { getApiClient } from '../../services/api';
import { t } from '../../i18n';
import { RebootNowButton } from '../common/RebootNowButton';
import { useCapabilities } from '../../core';
import { endpointRebootFields } from '../../core/schema/capabilities';
import { REBOOT_FIELDS_FALLBACK } from '../../core/shadow/ConfigShadow';

/** POSIX TZ presets (sign inverted vs UTC offset, as POSIX requires); label = phrase key. */
const TZ_PRESETS: { tz: string; label: string }[] = [
  { tz: 'GMT0BST,M3.5.0/1,M10.5.0', label: 'time.tzWesternEurope' },
  { tz: 'CET-1CEST,M3.5.0,M10.5.0/3', label: 'time.tzCentralEurope' },
  { tz: 'EET-2EEST,M3.5.0/3,M10.5.0/4', label: 'time.tzEasternEurope' },
  { tz: 'EET-2', label: 'time.tzKaliningrad' },
  { tz: 'MSK-3', label: 'time.tzMoscow' },
  { tz: '<+03>-3', label: 'time.tzIstanbulMinsk' },
  { tz: '<+04>-4', label: 'time.tzSamara' },
  { tz: '<+05>-5', label: 'time.tzYekaterinburg' },
  { tz: 'IST-5:30', label: 'time.tzIndia' },
  { tz: '<+06>-6', label: 'time.tzOmsk' },
  { tz: '<+07>-7', label: 'time.tzNovosibirsk' },
  { tz: '<+08>-8', label: 'time.tzIrkutsk' },
  { tz: 'CST-8', label: 'time.tzChina' },
  { tz: 'JST-9', label: 'time.tzJapan' },
  { tz: '<+09>-9', label: 'time.tzYakutsk' },
  { tz: '<+10>-10', label: 'time.tzVladivostok' },
  { tz: '<+11>-11', label: 'time.tzMagadan' },
  { tz: '<+12>-12', label: 'time.tzKamchatka' },
  { tz: 'EST5EDT,M3.2.0,M11.1.0', label: 'time.tzUsEastern' },
  { tz: 'CST6CDT,M3.2.0,M11.1.0', label: 'time.tzUsCentral' },
  { tz: 'MST7MDT,M3.2.0,M11.1.0', label: 'time.tzUsMountain' },
  { tz: 'PST8PDT,M3.2.0,M11.1.0', label: 'time.tzUsPacific' },
  { tz: 'UTC0', label: 'time.tzUtc' },
];

/** A zone's human name («Central Europe — Berlin…»), or the raw POSIX string. */
export const tzName = (tz: string): string => { const p = TZ_PRESETS.find((x) => x.tz === tz); return p ? t(p.label) : tz; };

/** This browser's zone (IANA name) and the POSIX string the hub needs for it — by its winter and
 *  summer offsets (wizard on a fresh hub 17.10, W191-04: «Berlin» was nowhere to be found). */
export function browserTz(now = new Date()): { iana: string; posix: string } | null {
  let iana = '';
  try { iana = Intl.DateTimeFormat().resolvedOptions().timeZone || ''; } catch { return null; }
  const y = now.getFullYear();
  const off = (m: number) => -new Date(y, m, 15, 12).getTimezoneOffset(); // minutes east of UTC
  const jan = off(0), jul = off(6);
  const known: Record<string, string> = {
    '0/60': 'GMT0BST,M3.5.0/1,M10.5.0', '60/120': 'CET-1CEST,M3.5.0,M10.5.0/3', '120/180': 'EET-2EEST,M3.5.0/3,M10.5.0/4',
    '-300/-240': 'EST5EDT,M3.2.0,M11.1.0', '-360/-300': 'CST6CDT,M3.2.0,M11.1.0', '-420/-360': 'MST7MDT,M3.2.0,M11.1.0', '-480/-420': 'PST8PDT,M3.2.0,M11.1.0',
  };
  const exact: Record<string, string> = { 'Europe/Moscow': 'MSK-3', 'Europe/Kaliningrad': 'EET-2', 'Asia/Tokyo': 'JST-9', 'Asia/Shanghai': 'CST-8', 'Asia/Kolkata': 'IST-5:30', 'Asia/Calcutta': 'IST-5:30' };
  if (exact[iana]) return { iana, posix: exact[iana] };
  const k = `${Math.min(jan, jul)}/${Math.max(jan, jul)}`;
  // Northern-hemisphere DST pairs above; southern zones and the rest fall back to a fixed offset.
  if (jan < jul && known[k]) return { iana, posix: known[k] };
  if (jan === jul) {
    if (jan === 0) return { iana, posix: 'UTC0' };
    const h = Math.trunc(jan / 60), m = Math.abs(jan % 60);
    const name = `${jan > 0 ? '+' : '-'}${String(Math.abs(h)).padStart(2, '0')}${m ? String(m).padStart(2, '0') : ''}`;
    return { iana, posix: `<${name}>${jan > 0 ? '-' : ''}${Math.abs(h)}${m ? `:${m}` : ''}` };
  }
  return null;
}

/**
 * The hub's `current_status` (rbjson.c: UNINITIALIZED, INITIALIZING, SYNCING, SYNCED, TIMEOUT, ERROR,
 * FALLBACK). Everything that isn't synced or on its way — a failed sync, a clock never set, an unknown
 * word — reads «not synchronized» (root 03.11: that was the state with no chip at all).
 */
export function syncStatusWord(status: unknown): { label: string; kind: 'live' | 'dormant' | 'error' } | null {
  if (status == null || status === '') return null;
  if (status === 'SYNCED') return { label: 'time.statusSynced', kind: 'live' };
  if (status === 'SYNCING' || status === 'INITIALIZING') return { label: 'time.statusSyncing', kind: 'dormant' };
  return { label: 'time.statusNotSynced', kind: 'error' };
}

type Cfg = Record<string, unknown>;

/** `compact` (the wizard): the zone and the save, without the NTP servers and interval. */
export const TimePanel: React.FC<{ compact?: boolean }> = ({ compact }) => {
  const [cfg, setCfg] = useState<Cfg | null>(null);
  const [draft, setDraft] = useState<Cfg | null>(null);
  const [stats, setStats] = useState<Cfg>({});
  const [busy, setBusy] = useState<string | null>(null);
  const [msg, setMsg] = useState<{ sev: 'success' | 'error' | 'warning'; text: string; reboot?: boolean } | null>(null);
  const [tried, setTried] = useState(false);
  const caps = useCapabilities();
  // The hub asks for a restart when the body CONTAINS an on-start field, changed or not (root
  // 18.10) — so only the changed fields go out, and the hint says which of them wait for a restart.
  const rebootFields = endpointRebootFields(caps, '/api/system/time-sync') ?? REBOOT_FIELDS_FALLBACK.timeSync;
  const changed = (): Cfg => {
    const out: Cfg = {};
    if (draft) for (const k of Object.keys(draft)) if (JSON.stringify(draft[k]) !== JSON.stringify(cfg?.[k])) out[k] = draft[k];
    return out;
  };

  const load = async () => {
    try {
      const env = await getApiClient().getTimeConfig();
      const data = (env as unknown as { data?: { config?: Cfg; statistics?: Cfg } }).data ?? {};
      setCfg(data.config ?? null);
      setDraft(data.config ?? null);
      setStats(data.statistics ?? {});
    } catch { /* offline */ } finally { setTried(true); }
  };
  useEffect(() => { void load(); }, []);

  const run = async (key: string, fn: () => Promise<Record<string, unknown>>, ok: string) => {
    setBusy(key);
    setMsg(null);
    try {
      const res = await fn();
      if (res.status && res.status !== 'OK') throw Object.assign(new Error(String(res.message ?? 'ERROR')), { body: res });
      setMsg(res.requires_reboot
        ? { sev: 'warning', text: t('time.okNeedsReboot', { ok }), reboot: true }
        : { sev: 'success', text: ok });
      await load();
    } catch (e) {
      const err = e as { status?: number; message?: string };
      setMsg({ sev: 'error', text: err.status === 401 || err.status === 403 ? t('common.adminTokenRequired') : t('time.failed', { message: err.message ?? t('time.deviceError') }) });
    } finally {
      setBusy(null);
    }
  };

  if (!draft) {
    return (
      <PanelCard title={t('time.title')}>
        <Typography variant="body2" color="text.secondary">{tried ? t('time.unavailable') : t('common.loading')}</Typography>
      </PanelCard>
    );
  }

  const suggest = browserTz();
  const status = syncStatusWord(stats.current_status);
  const tz = String(draft.timezone ?? '');
  const preset = TZ_PRESETS.find((p) => p.tz === tz);
  const dirty = JSON.stringify(draft) !== JSON.stringify(cfg);
  const set = (k: string, v: unknown) => setDraft({ ...draft, [k]: v });

  return (
    <PanelCard title={t('time.title')}>
      <Stack spacing={1.5}>
        <Stack direction="row" spacing={1} alignItems="center" flexWrap="wrap" useFlexGap>
          {status && <StatusChip status={status.kind} label={t(status.label)} />}
          {stats.active_source != null && (
            <Typography variant="body2" color="text.secondary">{t('time.source', { source: String(stats.active_source) })}</Typography>
          )}
        </Stack>

        <TextField
          select size="small" label={t('time.timezone')} value={preset ? tz : '__custom'}
          onChange={(e) => e.target.value !== '__custom' && set('timezone', e.target.value)}
        >
          {TZ_PRESETS.map((p) => <MenuItem key={p.tz} value={p.tz}>{t(p.label)}</MenuItem>)}
          <MenuItem value="__custom">{t('time.tzCustom')}{preset ? '' : `: ${tz}`}</MenuItem>
        </TextField>
        {!preset && <FormField label="POSIX TZ" value={tz} onChange={(v) => set('timezone', v)} />}
        {suggest && suggest.posix !== tz && (
          <Alert severity="info" action={<Button size="small" onClick={() => set('timezone', suggest.posix)}>{t('time.useBrowserTz')}</Button>}>
            {t('time.browserTzHint', { zone: suggest.iana || suggest.posix })}
          </Alert>
        )}
        {dirty && <Typography variant="caption" sx={{ color: 'warning.main' }}>{t('time.unsavedHint')}</Typography>}
        {dirty && Object.keys(changed()).some((k) => rebootFields.includes(k)) && (
          <Typography variant="caption" sx={{ color: 'warning.main' }}>{t('time.afterRebootHint')}</Typography>
        )}

        {!compact && <>
        <ToggleRow label={t('time.sntpEnabled')} checked={draft.enable_time_sync === true} onChange={(v) => set('enable_time_sync', v)} />
        <Box sx={{ display: 'grid', gap: 1.5, gridTemplateColumns: { xs: '1fr', sm: '1fr 1fr' } }}>
          <FormField label={t('time.ntpServer')} value={String(draft.sntp_server ?? '')} onChange={(v) => set('sntp_server', v)} />
          <FormField label={t('time.ntpServerBackup')} value={String(draft.sntp_server_backup ?? '')} onChange={(v) => set('sntp_server_backup', v)} />
        </Box>
        <FormField
          label={t('time.resyncInterval')} type="number" value={String(draft.resync_interval_sec ?? '')}
          onChange={(v) => set('resync_interval_sec', Number(v) || 0)}
        />
        </>}

        {msg && <Alert severity={msg.sev} action={msg.reboot ? <RebootNowButton /> : undefined}>{msg.text}</Alert>}

        <Stack direction="row" spacing={1} flexWrap="wrap" useFlexGap>
          <Button variant="contained" disabled={!dirty || !!busy}
            onClick={() => run('save', () => getApiClient().saveTimeConfig(changed()), t('time.saved'))}>
            {busy === 'save' ? t('common.saving') : t('common.save')}
          </Button>
          <Button variant="outlined" disabled={!!busy}
            onClick={() => run('ntp', () => getApiClient().triggerTimeSync(), t('time.ntpSynced'))}>
            {busy === 'ntp' ? t('time.syncing') : t('time.syncNtp')}
          </Button>
          <Button variant="text" disabled={!!busy}
            onClick={() => run('browser', () => getApiClient().syncTimeFromBrowser(tz || undefined), t('time.browserSynced'))}>
            {t('time.syncBrowser')}
          </Button>
        </Stack>
        <Typography variant="caption" color="text.secondary">
          {t('time.syncBrowserHint')}
        </Typography>
      </Stack>
    </PanelCard>
  );
};
