/**
 * EventsPage — «События» (spec §4.6). Two tabs: «Активные (N)» (alarms currently raised by
 * the device) and «Журнал» (the same current alarms + the hub's own log, `GET /api/v1/system/events`).
 *
 * Alarms are derived from live telemetry the same way AlarmsPanel does it (units flagged
 * `attributes['alarm'] === true`) — there is no dedicated /events endpoint yet. Severity is
 * ALWAYS shown as a WORD (инфо/внимание/серьёзно/критично), never colour alone (spec §9).
 * Acknowledgements and first-seen times are kept in localStorage, namespaced per device —
 * they never reach the hub (hence the «хранится только в этом браузере» note).
 */
import React, { useEffect, useMemo, useState } from 'react';
import { Box, Stack, Typography, Tabs, Tab, Button, Alert } from '@mui/material';
import { useNavigate } from 'react-router-dom';
import { Card } from '@rbgrid/design-system';
import { useDsTokens } from '../theme/useDsTokens';
import { dsFonts, DsPalette } from '../theme/dsTokens';
import { useTelemetryEntities } from '../hooks/useTelemetryEntities';
import { useLocalStorage } from '../hooks/useLocalStorage';
import { UnitEntityGroup, unitMetricValue, DEFAULT_SYSTEM_BASE } from '../entities/model';
import { pwW } from '../components/common/ruFormat';
import { intlLocale, t, dateLocale } from '../i18n';
import { useUnitsConfig } from '../hooks/useUnitsConfig';
import { offlinePoints, fullNameOf } from './overviewData';
import { isDemo } from '../services/connection';
import { useDeviceClock, formatDeviceTime } from '../core/deviceTime';
import { channelLabel } from '../entities/phaseGroup';
import { HubLog } from '../components/events/HubLog';
import { modulePollFailSince, ModuleFail } from '../components/events/modulePollFail';
import { getApiClient } from '../services/api';

/**
 * A silent point is an event too (UX audit HOME-06/RES-14: all modules offline and «Активных
 * тревог нет ✓»). Severity «внимание»; says which module/channel and what to check (INP-14).
 */
/** «14:05» of a browser moment (ms) on the HUB's clock — the boot line already is (3φ audit H3P-15:
 *  «restarted 18:49» beside «seen since 17:53» mixed the hub's and the browser's zones). */
const useHubHm = () => {
  const clock = useDeviceClock();
  return (ms?: number | null): string => (ms ? formatDeviceTime(clock, Math.floor(ms / 1000), { hour: '2-digit', minute: '2-digit' }, dateLocale()) : '—');
};

const OfflineCard: React.FC<{ group: UnitEntityGroup; name: string; since?: number; hubFail?: ModuleFail; onOpen: () => void }> = ({ group, name, since, hubFail, onOpen }) => {
  const ds = useDsTokens();
  const hubHm = useHubHm();
  const clock = useDeviceClock();
  const sev = sevStyle(ds, 1);
  const a = group.attributes ?? {};
  const where = typeof a.module === 'number'
    ? t('events.offlineWhere', { module: a.module as number, ch: typeof a.ch === 'number' ? channelLabel(a.ch as number, a.ch_label as string | undefined) : '—' })
    : null;
  return (
    <Card>
      <Stack direction="row" spacing={1.5} alignItems="flex-start"
        sx={{ p: 1, borderRadius: 1, border: `1px solid ${sev.color}`, bgcolor: sev.soft }}>
        <Box sx={{ color: sev.color, fontSize: 18, lineHeight: 1.2 }}>○</Box>
        <Box sx={{ flex: 1, minWidth: 0 }}>
          <Stack direction="row" spacing={1} alignItems="baseline" flexWrap="wrap">
            <Typography sx={{ fontWeight: 700, fontSize: 11, textTransform: 'uppercase', color: sev.color, letterSpacing: '0.04em' }}>
              {t(sev.word)}
            </Typography>
            <Typography sx={{ fontWeight: 600, fontSize: 14, color: ds.ink }} noWrap>{name}</Typography>
          </Stack>
          <Typography sx={{ fontSize: 13, color: ds.ink }}>{t('events.offlineTitle')}{where ? ` · ${where}` : ''}</Typography>
          <Typography sx={{ fontSize: 12, color: ds.inkMuted }}>{t('events.offlineHint')}</Typography>
          {/* The hub's log knows when polling the module failed (root 16.10); a module not found at
              start-up is never polled and has no such line — then what this browser saw. */}
          {hubFail ? (
            <Typography sx={{ fontSize: 11, color: ds.inkMuted, fontFamily: dsFonts.mono, mt: 0.25, ...tnum }}>
              {t(hubFail.kind === 'boot' ? 'events.hubNotFoundSince' : 'events.hubFailSince', { time: Date.now() / 1000 - hubFail.ts > 20 * 3600
                ? formatDeviceTime(clock, hubFail.ts, { day: 'numeric', month: 'short', hour: '2-digit', minute: '2-digit' }, dateLocale())
                : hubHm(hubFail.ts * 1000) })}
            </Typography>
          ) : since ? (
            <Typography sx={{ fontSize: 11, color: ds.inkMuted, fontFamily: dsFonts.mono, mt: 0.25, ...tnum }}>
              {t('events.seenSince', { time: hubHm(since) })}
            </Typography>
          ) : null}
          <Button size="small" variant="outlined" sx={{ mt: 1 }} onClick={onOpen}>{t('events.openPoint')}</Button>
        </Box>
      </Stack>
    </Card>
  );
};

/** A unit currently raising an alarm (same predicate as AlarmsPanel). */
function isAlarm(g: UnitEntityGroup): boolean {
  return g.attributes?.['alarm'] === true;
}

interface SevStyle {
  word: string;
  color: string;
  soft: string;
  icon: string;
}

/** Map a numeric severity (0 info … 3 critical) to its word, colour token and icon. */
function sevStyle(ds: DsPalette, sev: number): SevStyle {
  switch (sev) {
    case 0:
      return { word: 'events.sevInfo', color: ds.sevInfo, soft: ds.sevInfoSoft, icon: 'ℹ' };
    case 1:
      return { word: 'events.sevWarning', color: ds.sevWarning, soft: ds.sevWarningSoft, icon: '⚠' };
    case 3:
      return { word: 'events.sevCritical', color: ds.sevCritical, soft: ds.sevCriticalSoft, icon: '⛔' };
    case 2:
    default:
      return { word: 'events.sevAlarm', color: ds.sevAlarm, soft: ds.sevAlarmSoft, icon: '⚠' };
  }
}

/** Read an optional numeric attribute (severity/threshold/hysteresis) if the firmware sends it. */
function numAttr(g: UnitEntityGroup, key: string): number | undefined {
  const v = g.attributes?.[key];
  return typeof v === 'number' ? v : undefined;
}

const tnum = { fontVariantNumeric: 'tabular-nums' as const };

interface AlarmCardProps {
  group: UnitEntityGroup;
  /** Wall-clock ms the alarm was first seen in this browser (for «с ЧЧ:ММ»). */
  since?: number;
  /** Wall-clock ms this alarm was acknowledged in this browser, if any. */
  ackedAt?: number;
  onAck: () => void;
  onOpenPoint: () => void;
  onOpenRule: () => void;
}

const AlarmCard: React.FC<AlarmCardProps & { name: string }> = ({ group, name, since, ackedAt, onAck, onOpenPoint, onOpenRule }) => {
  const ds = useDsTokens();
  const sev = sevStyle(ds, numAttr(group, 'alarm_sev') ?? 2);

  const power = unitMetricValue(group, 'power');
  const details: string[] = [t('events.value', { value: pwW(power) })];
  const thr = numAttr(group, 'alarm_threshold');
  if (thr != null) details.push(t('events.threshold', { value: pwW(thr) }));
  const hys = numAttr(group, 'alarm_hyst');
  if (hys != null) details.push(t('events.hysteresis', { value: pwW(hys) }));

  const hhmm = useHubHm();

  return (
    <Card>
      <Stack
        direction="row"
        spacing={1.5}
        alignItems="flex-start"
        sx={{ p: 1, borderRadius: 1, border: `1px solid ${sev.color}`, bgcolor: sev.soft }}
      >
        <Box sx={{ color: sev.color, fontSize: 18, lineHeight: 1.2 }}>{sev.icon}</Box>

        <Box sx={{ flex: 1, minWidth: 0 }}>
          <Stack direction="row" spacing={1} alignItems="baseline" flexWrap="wrap">
            <Typography sx={{ fontWeight: 700, fontSize: 11, textTransform: 'uppercase', color: sev.color, letterSpacing: '0.04em' }}>
              {t(sev.word)}
            </Typography>
            <Typography sx={{ fontWeight: 600, fontSize: 14, color: ds.ink }} noWrap>
              {name}
            </Typography>
          </Stack>

          <Typography sx={{ fontSize: 12, color: ds.inkMuted, ...tnum }}>
            {details.join(' · ')}
          </Typography>

          <Typography sx={{ fontSize: 11, color: ds.inkMuted, fontFamily: dsFonts.mono, mt: 0.25, ...tnum }}>
            {t('events.since', { time: hhmm(since) })}
          </Typography>

          <Stack direction="row" spacing={1} alignItems="center" flexWrap="wrap" sx={{ mt: 1 }}>
            <Button size="small" variant="outlined" onClick={onOpenPoint}>{t('events.openPoint')}</Button>
            <Button size="small" variant="text" onClick={onOpenRule}>{t('events.rule')}</Button>
            {ackedAt ? (
              <Typography sx={{ fontSize: 12, color: ds.statusOnline, fontWeight: 600, ...tnum }}>
                {t('events.ackedAt', { time: hhmm(ackedAt) })}
              </Typography>
            ) : (
              <Button size="small" variant="contained" onClick={onAck}>{t('events.ack')}</Button>
            )}
          </Stack>
          {!ackedAt && (
            <Typography sx={{ fontSize: 10.5, color: ds.inkMuted, mt: 0.5 }}>
              {t('events.ackLocalOnly')}
            </Typography>
          )}
        </Box>
      </Stack>
    </Card>
  );
};

/** The boot reason in words (the hub's codes; an unknown one is shown as is). */
const bootReason = (r?: string): string => {
  const k = `events.bootReason_${String(r ?? '').replace(/[^a-z]+/gi, '_').toLowerCase()}`;
  const v = t(k);
  return v !== k ? v : (r || t('events.bootReason_unknown'));
};
const downText = (s: number): string => (s < 90 ? t('events.durS', { s: Math.round(s) }) : s < 5400 ? t('events.durM', { m: Math.round(s / 60) }) : t('events.durH', { h: Math.round(s / 360) / 10 }));

type EventsTab = 'active' | 'log';

export const EventsPage: React.FC = () => {
  const ds = useDsTokens();
  const navigate = useNavigate();
  const { data, lastUpdate } = useTelemetryEntities();
  const clock = useDeviceClock();
  const hubHm = useHubHm();
  const boot = data?.system.totals?.boot;
  const [tab, setTab] = useState<EventsTab>('active');
  // Without fresh frames the alarm state is UNKNOWN — never «тревог нет ✓» (rerun 16.10 SCADA-R01:
  // 4 active alarms before the drop, «нет активных» after it). 15 s without a frame = lost.
  const [nowMs, setNowMs] = useState(() => Date.now());
  useEffect(() => { const id = setInterval(() => setNowMs(Date.now()), 5000); return () => clearInterval(id); }, []);
  const lost = !isDemo() && (lastUpdate == null || nowMs - lastUpdate > 15000);

  const deviceKey = data?.system.deviceId ?? DEFAULT_SYSTEM_BASE;
  const [acks, setAcks] = useLocalStorage<Record<string, number>>(`rbgrid.events.acks.${deviceKey}`, {});
  const [seen, setSeen] = useLocalStorage<Record<string, number>>(`rbgrid.events.seen.${deviceKey}`, {});

  const alarms = useMemo(() => (data?.units ?? []).filter(isAlarm), [data]);
  const offline = useMemo(() => (data ? offlinePoints(data.units) : []), [data]);
  const unitsCfg = useUnitsConfig();
  const offlineKeys = offline.map((g) => g.unitKey).join(',');
  // First time each silent point was seen silent in this browser (the hub has no event log yet).
  useEffect(() => {
    if (offline.length === 0) return;
    const now = Date.now();
    setSeen((prev) => {
      let changed = false;
      const next = { ...prev };
      for (const g of offline) {
        const k = `offline:${g.unitKey}`;
        if (next[k] == null) { next[k] = now; changed = true; }
      }
      return changed ? next : prev;
    });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [offlineKeys]);
  const total = alarms.length + offline.length;

  // When polling each silent module failed, by the hub's log; re-read each minute while any is silent.
  const [failSince, setFailSince] = useState<Map<number, ModuleFail>>(new Map());
  const anyOffline = offline.length > 0;
  useEffect(() => {
    if (!anyOffline) return undefined;
    let live = true;
    const read = () => getApiClient().getHubEventsPage({ level: 'event', limit: 1000 })
      .then((p) => { if (live) setFailSince(modulePollFailSince(p.events)); })
      .catch(() => { /* no log on this firmware — the browser's own time stays */ });
    read();
    const id = setInterval(read, 60_000);
    return () => { live = false; clearInterval(id); };
  }, [anyOffline]);
  const alarmKeys = alarms.map((g) => g.unitKey).join(',');

  // Record the first wall-clock time each alarm appeared (persisted per device) → «с ЧЧ:ММ».
  useEffect(() => {
    if (alarms.length === 0) return;
    const now = Date.now();
    setSeen((prev) => {
      let changed = false;
      const next = { ...prev };
      for (const g of alarms) {
        if (next[g.unitKey] == null) { next[g.unitKey] = now; changed = true; }
      }
      return changed ? next : prev;
    });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [alarmKeys]);

  const ackOne = (unitKey: string) => setAcks((prev) => ({ ...prev, [unitKey]: Date.now() }));

  const renderAlarms = () => (
    <Stack spacing={1.25}>
      {offline.map((g) => (
        <OfflineCard key={`off-${g.unitKey}`} group={g} name={fullNameOf(g, unitsCfg)} since={seen[`offline:${g.unitKey}`]}
          hubFail={typeof g.attributes?.module === 'number' ? failSince.get(g.attributes.module as number) : undefined}
          onOpen={() => navigate(`/point/${g.unitKey}`)} />
      ))}
      {alarms.map((g) => (
        <AlarmCard
          key={g.unitKey}
          group={g}
          name={fullNameOf(g, unitsCfg)}
          since={seen[g.unitKey]}
          ackedAt={acks[g.unitKey]}
          onAck={() => ackOne(g.unitKey)}
          onOpenPoint={() => navigate(`/point/${g.unitKey}`)}
          onOpenRule={() => navigate('/config')}
        />
      ))}
    </Stack>
  );

  const emptyState = (
    <Card>
      <Stack direction="row" spacing={1} alignItems="center" sx={{ py: 1.5 }}>
        <Box sx={{ color: ds.statusOnline, fontSize: 20, lineHeight: 1 }}>✓</Box>
        <Typography sx={{ color: ds.inkMuted }}>{t('events.noActiveAlarms')}</Typography>
      </Stack>
    </Card>
  );

  return (
    <Box>
      <Typography variant="h4" fontWeight={800} sx={{ mb: 2 }}>{t('events.title')}</Typography>

      <Tabs
        value={tab}
        onChange={(_, v: EventsTab) => setTab(v)}
        sx={{
          mb: 2.5,
          minHeight: 40,
          '& .MuiTab-root': { minHeight: 40, textTransform: 'none', color: ds.inkMuted, fontWeight: 600 },
          '& .MuiTab-root.Mui-selected': { color: ds.ink },
          '& .MuiTabs-indicator': { backgroundColor: ds.primary },
        }}
      >
        <Tab value="active" label={t('events.tabActive', { n: total })} />
        <Tab value="log" label={t('events.tabLog')} />
      </Tabs>

      {lost && (
        <Alert severity="warning" sx={{ mb: 2 }}>
          {lastUpdate ? t('events.lostSince', { time: hubHm(lastUpdate) }) : t('events.lostNever')}
        </Alert>
      )}
      {/* The hub's last restart, from the hub itself (system.boot) — when it happened, why and how
          long it was down; the history around it may be incomplete (SCADA-R05, root 33acbedd). */}
      {boot && boot.ts && boot.ts > 1_600_000_000 && Date.now() / 1000 - boot.ts < 7 * 86400 && (
        <Alert severity={boot.planned ? 'info' : 'warning'} sx={{ mb: 2 }}>
          {t('events.bootLine', {
            time: formatDeviceTime(clock, boot.ts, { day: 'numeric', month: 'short', hour: '2-digit', minute: '2-digit' }, dateLocale()),
            reason: bootReason(boot.reason),
          })}
          {boot.downtime_s && boot.downtime_s > 60 ? ` ${t('events.bootDown', { dur: downText(boot.downtime_s) })}` : ''}
        </Alert>
      )}
      {tab === 'active' && (total === 0 ? (lost ? null : emptyState) : renderAlarms())}

      {tab === 'log' && (
        <Stack spacing={1.25}>
          <Typography sx={{ fontSize: 12, color: ds.inkMuted }}>
            {t('events.logNote')}
          </Typography>
          {total === 0 ? (lost ? null : emptyState) : renderAlarms()}
          <Box sx={{ pt: 1.5 }}><HubLog /></Box>
        </Stack>
      )}
    </Box>
  );
};
