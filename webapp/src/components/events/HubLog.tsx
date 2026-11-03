/**
 * HubLog — the hub's own event log («События → Журнал», root canon 10723b74): restarts and their
 * reasons, OTA, Wi-Fi changes, module poll failures, hour/day closing. A live hub has only Wi-Fi
 * and OTA (no USB console) — this is where support reads what happened. `msg` is a technical line
 * and is shown as is (root: not parsed); time on the hub's clock, lines grouped by boot.
 */
import React, { useCallback, useEffect, useMemo, useState } from 'react';
import { Box, Stack, Typography, Button, Alert, ToggleButtonGroup, ToggleButton } from '@mui/material';
import { useDsTokens } from '../../theme/useDsTokens';
import { dsFonts } from '../../theme/dsTokens';
import { getApiClient } from '../../services/api';
import { HubEvent, HubEventsPage, HubVersion } from '../../types/hubEvents';
import { useDeviceClock, formatDeviceTime } from '../../core/deviceTime';
import { intlLocale, t, dateLocale } from '../../i18n';

type Level = 'fail' | 'event' | 'all';

/** How far back (in seq, all levels counted) one load reaches; «Earlier» steps back by the same. */
const WINDOW = 3000;
/** Rows kept on screen. */
const KEEP = 600;

export const HubLog: React.FC = () => {
  const ds = useDsTokens();
  const clock = useDeviceClock();
  const [level, setLevel] = useState<Level>('event');
  const [rows, setRows] = useState<HubEvent[]>([]);
  const [from, setFrom] = useState<number | null>(null);
  const [meta, setMeta] = useState<HubEventsPage | null>(null);
  const [ver, setVer] = useState<HubVersion | null>(null);
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState(false);
  const [copied, setCopied] = useState(false);

  const load = useCallback(async (lvl: Level) => {
    setBusy(true);
    setErr(false);
    try {
      const api = getApiClient();
      const tail = await api.getHubEventsPage({ level: lvl, limit: 1 });
      const start = Math.max(0, tail.seq_last - WINDOW);
      const { events, page } = await api.getHubEventsRange(lvl, start, tail.seq_last);
      setRows(events.slice(-KEEP));
      setFrom(start);
      setMeta(page ?? tail);
    } catch {
      setErr(true);
    } finally {
      setBusy(false);
    }
  }, []);

  const earlier = async () => {
    if (from == null || from <= 0) return;
    setBusy(true);
    try {
      const start = Math.max(0, from - WINDOW);
      const { events } = await getApiClient().getHubEventsRange(level, start, from);
      setRows((r) => [...events, ...r].slice(0, KEEP * 2));
      setFrom(start);
    } catch {
      setErr(true);
    } finally {
      setBusy(false);
    }
  };

  useEffect(() => { load(level); }, [level, load]);
  useEffect(() => {
    getApiClient().getHubVersion().then(setVer).catch(() => setVer(null));
  }, []);

  const when = useCallback((e: HubEvent): string => (e.ts > 1_600_000_000
    ? formatDeviceTime(clock, e.ts, { day: 'numeric', month: 'short', hour: '2-digit', minute: '2-digit', second: '2-digit' }, dateLocale())
    : t('hubLog.upTime', { s: e.up })), [clock]);

  // Newest first, a header row at each boot change.
  const view = useMemo(() => [...rows].reverse(), [rows]);
  const text = useMemo(() => view.map((e) => `${e.seq}\tb${e.b}\t${when(e)}\t${e.lvl}\t${e.msg}`).join('\n'), [view, when]);

  const copy = async () => {
    try {
      const head = ver ? `fw ${ver.version ?? '?'} image ${(ver.image_md5 ?? '').slice(0, 8)} ${ver.partition ?? ''}\n` : '';
      await navigator.clipboard.writeText(head + text);
      setCopied(true);
      setTimeout(() => setCopied(false), 2000);
    } catch { /* clipboard blocked — nothing to do */ }
  };

  const lvlWord = (l: HubEvent['lvl']) => t(l === 'fail' ? 'hubLog.lvlFail' : l === 'event' ? 'hubLog.lvlEvent' : 'hubLog.lvlPeriodic');

  return (
    <Stack spacing={1.25}>
      <Typography sx={{ fontWeight: 700, fontSize: 15, color: ds.ink }}>{t('hubLog.title')}</Typography>
      <Typography sx={{ fontSize: 12, color: ds.inkMuted }}>{t('hubLog.intro')}</Typography>
      {ver && (ver.version || ver.image_md5) && (
        <Typography sx={{ fontSize: 12, color: ds.inkMuted, fontFamily: dsFonts.mono }}>
          {t('hubLog.firmware', { version: ver.version ?? '—', image: (ver.image_md5 ?? '—').slice(0, 8), partition: ver.partition ?? '—' })}
        </Typography>
      )}
      <Stack direction="row" spacing={1} alignItems="center" flexWrap="wrap" useFlexGap>
        <ToggleButtonGroup size="small" exclusive value={level} onChange={(_, v: Level | null) => v && setLevel(v)}>
          <ToggleButton value="fail" sx={{ textTransform: 'none' }}>{t('hubLog.showFail')}</ToggleButton>
          <ToggleButton value="event" sx={{ textTransform: 'none' }}>{t('hubLog.showEvent')}</ToggleButton>
          <ToggleButton value="all" sx={{ textTransform: 'none' }}>{t('hubLog.showAll')}</ToggleButton>
        </ToggleButtonGroup>
        <Button size="small" variant="outlined" disabled={busy} onClick={() => load(level)}>{t('hubLog.refresh')}</Button>
        <Button size="small" variant="outlined" disabled={!rows.length} onClick={copy}>{copied ? t('hubLog.copied') : t('hubLog.copy')}</Button>
      </Stack>
      {meta && meta.fail_boot > 0 && <Alert severity="warning">{t('hubLog.failBoot', { n: meta.fail_boot })}</Alert>}
      {meta && !meta.persist && <Alert severity="info">{t('hubLog.noPersist')}</Alert>}
      {err && <Alert severity="warning">{t('hubLog.error')}</Alert>}
      {busy && !rows.length && <Typography sx={{ fontSize: 13, color: ds.inkMuted }}>{t('hubLog.loading')}</Typography>}
      {!busy && !err && !rows.length && <Typography sx={{ fontSize: 13, color: ds.inkMuted }}>{t('hubLog.empty')}</Typography>}
      {rows.length > 0 && (
        <Box sx={{ border: `1px solid ${ds.border}`, borderRadius: 2, maxHeight: 520, overflow: 'auto' }}>
          {view.map((e, i) => {
            const bootHead = i === 0 || view[i - 1].b !== e.b;
            const fail = e.lvl === 'fail';
            return (
              <React.Fragment key={e.seq}>
                {bootHead && (
                  <Typography sx={{ px: 1.25, py: 0.5, fontSize: 11, fontWeight: 700, color: ds.inkMuted, bgcolor: ds.surfaceRaised, textTransform: 'uppercase', letterSpacing: '0.04em' }}>
                    {t(e.b === meta?.boot ? 'hubLog.bootCurrent' : 'hubLog.boot', { n: e.b })}
                  </Typography>
                )}
                <Stack direction={{ xs: 'column', sm: 'row' }} spacing={{ xs: 0, sm: 1.25 }}
                  sx={{ px: 1.25, py: 0.4, borderTop: `1px solid ${ds.border}`, bgcolor: fail ? ds.sevAlarmSoft : 'transparent' }}>
                  <Typography sx={{ fontSize: 12, color: ds.inkMuted, fontVariantNumeric: 'tabular-nums', whiteSpace: 'nowrap', minWidth: 150 }}>{when(e)}</Typography>
                  <Typography sx={{ fontSize: 11, fontWeight: 700, color: fail ? ds.sevAlarm : ds.inkMuted, textTransform: 'uppercase', minWidth: 64, pt: '1px' }}>{lvlWord(e.lvl)}</Typography>
                  <Typography sx={{ fontSize: 12, color: ds.ink, fontFamily: dsFonts.mono, wordBreak: 'break-word', minWidth: 0 }}>{e.msg}</Typography>
                </Stack>
              </React.Fragment>
            );
          })}
        </Box>
      )}
      {from != null && from > 0 && rows.length > 0 && (
        <Box><Button size="small" disabled={busy} onClick={earlier}>{t('hubLog.earlier')}</Button></Box>
      )}
    </Stack>
  );
};
