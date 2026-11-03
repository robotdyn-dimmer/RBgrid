/**
 * PointDetailPage — one measurement point (spec §7 «Точка»), mounted at /point/:unitKey.
 *
 * Header (role icon in a 44px frame · роль · источник · статус, caps spelled out in words);
 * live values gated to the source's measured field set (fields_by_source, U3 — no 0.00 for
 * fields a source doesn't measure); a token-driven inline-SVG trend with period/type switches;
 * today/total counters; a mono «Технические детали» block; and the delete action last, in
 * sev-critical. For an EV station the current session (status-from-power) is added. Plain SVG,
 * ru-RU, dark tokens — consistent with the redesigned Overview.
 */
import React, { useMemo, useState, useRef } from 'react';
import { dayAvgRate, solarValue } from '../economics/costRate';
import { useDeviceClock, formatDeviceTime } from '../core/deviceTime';
import { useParams, Link as RouterLink, useNavigate, useLocation } from 'react-router-dom';
import { Box, Stack, Typography, Button, ToggleButton, ToggleButtonGroup, CircularProgress, Alert, Link, Accordion, AccordionSummary, AccordionDetails } from '@mui/material';
import ArrowBackIcon from '@mui/icons-material/ArrowBack';
import ExpandMoreIcon from '@mui/icons-material/ExpandMore';
import { Card } from '@rbgrid/design-system';
import { useTelemetryEntities } from '../hooks/useTelemetryEntities';
import { useCapabilities, useConfigShadow } from '../core';
import { isFieldMeasured } from '../core/schema/capabilities';
import { NoHistoryNote } from '../components/common/NoHistoryNote';
import { useUnitsConfig, phaseLabel } from '../hooks/useUnitsConfig';
import { useGridSystem } from '../hooks/useGridSystem';
import { linePath, runsOf, Curve } from '../components/charts/chartPath';
import { HoverLayer } from '../components/charts/HoverLayer';
import { nominalFor, voltageBand } from '../entities/gridSystem';
import { useHistory } from '../hooks/useHistory';
import { HistoryElectricalRecord, HistoryEnergyRecord, HistoryPeriod, HistoryType } from '../types/history';
import { UnitEntityGroup, unitMetricValue } from '../entities/model';
import { useDsTokens } from '../theme/useDsTokens';
import { dsFonts } from '../theme/dsTokens';
import { pwW, curA, volt, enWh, nf, reactive } from '../components/common/ruFormat';
import { isStation, fullNameOf, roleLabel, isGenerationUnit } from './overviewData';
import { apiUnitId, isSupplyRole, VBUS_KEY } from '../entities/siteModel';
import { deriveStationStatus, STATION_STATUS, stationStatusTooltip, evThresholds, stationThresholds } from '../components/stations/evStatus';
import { EvConfig } from '../types/config';
import { useElementWidth } from '../hooks/useElementWidth';
import { UnitParamsPanel } from '../components/devices/UnitParamsPanel';
import { intlLocale, t, tCode, dateLocale } from '../i18n';
import { channelLabel } from '../entities/phaseGroup';
import { useIsBasicPoint, useIsCurrentOnlyPoint, useHardware } from '../hooks/useHardware';
import { pointHardware, ctRatingOf, basicReversed } from '../entities/moduleLine';
import { sctUncorrected } from '../types/sensors';
import { useConfirm } from '../components/common/ConfirmDialog';
import { useMoneyFormat } from '../hooks/useMoneyFormat';
import { getApiClient } from '../services/api';
import { WALLCLOCK_TS } from '../entities/history';
import { historyHref, winForPeriod } from './historyLink';

const ICON: Record<string, string> = {
  plug: 'M9 2v6M15 2v6M6 8h12v4a6 6 0 0 1-12 0zM12 18v4',
  sun: 'M12 17a5 5 0 1 0 0-10 5 5 0 0 0 0 10zM12 1v2M12 21v2M4.2 4.2l1.4 1.4M18.4 18.4l1.4 1.4M1 12h2M21 12h2M4.2 19.8l1.4-1.4M18.4 5.6l1.4-1.4',
  drop: 'M12 2.7l5.7 5.7a8 8 0 1 1-11.4 0z',
  flame: 'M12 2c1 3 5 5 5 10a5 5 0 0 1-10 0c0-3 2-4 2-7 1 1 2 2 3 4',
  bulb: 'M9 18h6M10 22h4M12 2a7 7 0 0 0-4 12.7V17h8v-2.3A7 7 0 0 0 12 2z',
  car: 'M3 13l2-6h14l2 6v4H3zM6 17v2M18 17v2M7 13h.01M17 13h.01',
  tower: 'M12 2v20M5 22l7-20 7 20M7 9h10M6 14h12',
};
function iconFor(g: UnitEntityGroup): string {
  const n = `${g.point ?? ''} ${g.name}`.toLowerCase();
  if (/(ev|charg|заряд|авто|car)/.test(n)) return 'car';
  if (/(solar|солн|pv|генер)/.test(n)) return 'sun';
  if (/(boiler|бойлер|water|вода|hot)/.test(n)) return 'drop';
  if (/(heat|отопл|flame|печ|furnace)/.test(n)) return 'flame';
  if (/(light|освещ|lamp|свет)/.test(n)) return 'bulb';
  if (/(main|ввод|grid|сеть)/.test(n)) return 'tower';
  return 'plug';
}

const SRC_LABEL: Record<string, string> = { rbamp: 'rbAmp', espnow: 'ESP-NOW' };
/** History energy is ALWAYS float Wh on the wire (canon fc0f962b). */
const toKwh = (wh: number): number => wh / 1000;

/** rbAmp channel: 0..n = a current channel; 254 = the phase group (all current channels); 255 = the voltage input. */
/** «I0 (ch 0)», «L1–L3 (ch 254)»: the label printed on the module, the raw API `ch` beside it. */
const chText = (ch: unknown, label?: unknown): string =>
  typeof ch === 'number' ? `${channelLabel(ch, typeof label === 'string' ? label : null)} (ch ${ch})` : '—';

type ChartType = 'power' | 'voltage' | 'energy';

/** A scale figure: «1 034 Вт», «45,6 Вт», «0,35 кВт·ч» — not «1 034,0 Вт» (16.10, .110 point page). */
const scaleNum = (v: number): string => nf(v, Math.abs(v) >= 100 ? 0 : Math.abs(v) >= 1 ? 1 : 2);

export const PointDetailPage: React.FC = () => {
  const { unitKey = '' } = useParams<{ unitKey: string }>();
  const setupOpen = /[?&]setup\b/.test(useLocation().search);
  const setupScrolled = useRef(false);
  const { data } = useTelemetryEntities();
  const caps = useCapabilities();
  const unitsCfg = useUnitsConfig();
  const sysShadow = useConfigShadow('system'); // system.ev: the hub's station thresholds
  // BASIC module (root dd861a54): consumption only, accuracy floor ≈1.5 % of the CT.
  const basicPoint = useIsBasicPoint(unitKey);
  const currentOnly = useIsCurrentOnlyPoint(unitKey); // I1/I2/I3: monitoring, not metering
  const hwAll = useHardware();
  const hwCtRating = ctRatingOf(pointHardware(unitKey, hwAll.sensors, hwAll.modules).sensor, caps.ctModels);

  const grid = useGridSystem(unitsCfg);
  const ds = useDsTokens();
  const navigate = useNavigate();
  const mf = useMoneyFormat(data?.system.totals?.tariff?.currency);

  const [period, setPeriod] = useState<HistoryPeriod>('hour');
  const [ctype, setCtype] = useState<ChartType>('power');
  const [delBusy, setDelBusy] = useState(false);
  const [delErr, setDelErr] = useState<string | null>(null);
  const confirm = useConfirm();

  // The route accepts the telemetry key ("main", "9010") or its REST HEX4 ("0006").
  const group = data?.units.find((u) => u.unitKey === unitKey)
    ?? data?.units.find((u) => (unitsCfg.forGroup(u)?.hex ?? apiUnitId(u.unitKey)) === (apiUnitId(unitKey) ?? unitKey).toUpperCase());
  const histType: HistoryType = ctype === 'energy' ? 'energy' : 'electrical';
  // REST takes HEX4 ids only ("main" → 0006); legacy telemetry keys are translated here.
  // The configured unit's HEX4 wins (an rbAmp input may still be keyed "main" in telemetry).
  const apiId = group ? (unitsCfg.forGroup(group)?.hex ?? apiUnitId(group.unitKey)) : null;
  const { records } = useHistory(apiId, period, histType);

  // A generation point's energy is what it PRODUCED (`es`), not «взято» (UX audit GEN-12).
  const genPoint = !!group && isGenerationUnit(group, unitsCfg);
  const series = useMemo(() => {
    if (ctype === 'energy') {
      const recs = (records as HistoryEnergyRecord[]).filter((r) => typeof r.ts === 'number' && r.ts >= WALLCLOCK_TS) /* no uptime-stamped / 1970 test records (INP-05) */;
      return [...recs].reverse().slice(-24).map((r) => ({ ts: r.ts, value: toKwh((genPoint ? r.es : r.ec) ?? 0) }));
    }
    const recs = (records as HistoryElectricalRecord[]).filter((r) => typeof r.ts === 'number' && r.ts >= WALLCLOCK_TS) /* no uptime-stamped / 1970 test records (INP-05) */;
    // A generation point's output is negative on the wire (export) — drawn as a positive hump,
    // not a noon «dip» (rerun 16.10 GEN-R09).
    return [...recs].reverse().slice(-48).map((r) => ({ ts: r.ts, value: ctype === 'power' ? (genPoint ? Math.abs(r.p_act) : r.p_act) : r.v }));
  }, [records, ctype, genPoint]);

  if (!data) {
    return (
      <Box sx={{ display: 'flex', justifyContent: 'center', py: 10 }}><CircularProgress aria-label={t('common.loading')} /></Box>
    );
  }

  if (!group) {
    return (
      <Box>
        <Typography variant="h4" fontWeight={700} sx={{ mb: 1 }}>{t('point.title')}</Typography>
        <Card>
          <Stack spacing={1.5} sx={{ py: 2 }} alignItems="flex-start">
            <Typography color="text.secondary">{t('point.notFound', { key: unitKey })}</Typography>
            <Button component={RouterLink} to="/devices" variant="outlined" size="small">{t('point.backToList')}</Button>
          </Stack>
        </Card>
      </Box>
    );
  }

  const cfg = unitsCfg.forGroup(group);
  // The input and the voltage bus anchor the whole site — not deletable from here (PNT-17).
  const deleteLocked = isSupplyRole(cfg?.role) || cfg?.type === 'VOLTAGE_BUS' || group.unitKey === VBUS_KEY;
  // Real delete (UX audit PNT-01: the confirm used to only navigate away, the point stayed).
  const deletePoint = async () => {
    if (!apiId) return;
    const ok = await confirm({
      title: t('point.deleteConfirm', { name: fullNameOf(group, unitsCfg) }),
      body: t('point.deleteBody'), confirmLabel: t('common.delete'), danger: true,
    });
    if (!ok) return;
    setDelBusy(true); setDelErr(null);
    try {
      const res = await getApiClient().deleteUnit(apiId);
      if (res.status === 'ok') { navigate('/devices'); return; }
      setDelErr(t('point.deleteFailed', { message: res.message ?? '—' }));
    } catch {
      setDelErr(t('point.deleteRequestFailed'));
    } finally {
      setDelBusy(false);
    }
  };
  const station = isStation(group, unitsCfg);
  const power = unitMetricValue(group, 'power');
  const roleText = station ? t('point.roleStation') : cfg?.role ? roleLabel(cfg.role) : t('point.roleLoad');
  const srcText = basicPoint ? 'rbAmp BASIC' : SRC_LABEL[group.srcClass] ?? group.srcClass;

  // caps → words (spec §7)
  const capWords: string[] = [];
  // An rbAmp channel signs its power (root f451b6a5: − = export) — it HAS a direction even
  // without the DIRECTION cap (GEN-R06: the solar panels read «без направления»).
  if (currentOnly) capWords.push(t('point.capCurrentOnly'));
  else capWords.push(basicPoint ? t('point.capBasic')
    : group.hasDirection || group.srcClass === 'rbamp' ? t('point.capDirection') : t('point.capNoDirection'));
  if ((group.caps ?? []).some((c) => c === 'PERSISTED' || c === 'DEVICE_ENERGY')) capWords.push(t('point.capPersisted'));
  if (cfg?.phase) capWords.push(t('point.capPhase', { phase: phaseLabel(cfg.phase) }));

  const statusText = !group.present
    ? (group.noChannel ? t('cards.noChannel') : t('common.noData'))
    : station
      ? STATION_STATUS[deriveStationStatus(power, group.present, null, stationThresholds(evThresholds(sysShadow.reported.ev as unknown as EvConfig | undefined), cfg))].label
      : t('point.statusNormal');
  const statusColor = !group.present ? ds.statusOffline : ds.statusOnline;

  // live values gated by fields_by_source for this unit's src_class
  const showPf = isFieldMeasured(caps, group.srcClass, 'pf');
  const showRea = isFieldMeasured(caps, group.srcClass, 'p_rea');
  // Voltage outside the nominal ±10 % (a two-pole line sees line-to-line) → flagged.
  const voltage = unitMetricValue(group, 'voltage');
  const vBand = voltageBand(voltage, nominalFor(grid, cfg?.phase));
  const liveStats: { label: string; value: string; color?: string }[] = [
    vBand
      ? { label: t('point.voltage'), value: `${volt(voltage)} · ${t(vBand === 'low' ? 'cards.voltLow' : 'cards.voltHigh')}`, color: ds.sevAlarm }
      : { label: (group.phases?.length ?? 0) > 1 ? t('point.voltageMean') : t('point.voltage'), value: volt(voltage) },
    // A 3φ input's i is the Σ of its phase currents (U3I3): no conductor carries it — say so, and
    // give the U as the phases' mean (the per-phase table below has the real figures).
    { label: (group.phases?.length ?? 0) > 1 ? t('point.currentSum') : t('point.current'), value: curA(unitMetricValue(group, 'current')) },
    // Negative p_act = export (current firmware: the sign is the direction) → «↑ 478 Вт».
    power != null && power < 0
      ? { label: t('point.power'), value: '↑ ' + pwW(Math.abs(power)), color: ds.flowExport }
      : { label: t('point.power'), value: pwW(power) },
  ];
  if (showRea) liveStats.push({ label: t('point.reactive'), value: reactive(unitMetricValue(group, 'reactive_power') ?? 0, power) });
  if (showPf) {
    const pf = unitMetricValue(group, 'power_factor');
    liveStats.push({ label: t('units.pf'), value: pf != null ? nf(pf, 2) : '—' });
  }

  // A point without data reports zeros — unknown, not «0 кВт·ч» (rerun 16.10 PNT-R06).
  const known = (v: number | null) => (!group.present && (v == null || v === 0) ? null : v);
  const dayIn = known(unitMetricValue(group, 'energy_day_in'));
  const dayOut = known(unitMetricValue(group, 'energy_day_out'));
  const totIn = known(unitMetricValue(group, 'energy_in'));
  const totOut = known(unitMetricValue(group, 'energy_out'));

  const mono = { fontFamily: dsFonts.mono, fontVariantNumeric: 'tabular-nums' as const };

  // Today's money of the point at the input's AVERAGE price of the day (rerun 16.10 R-T1 — the
  // current zone's rate made the kitchen 4,82 ₽ here vs 1,92 ₽ on the Overview); the live rate
  // only while the hub has no money for the day yet.
  const liveR = data.system.totals?.tariff;
  const avg = dayAvgRate(data.system.totals);
  const buy = avg.buy ?? (liveR?.buy_rate && liveR.buy_rate > 0 ? liveR.buy_rate : null);
  const sell = avg.sell ?? (liveR?.sell_rate && liveR.sell_rate > 0 ? liveR.sell_rate : null);
  const dayMoneyIn = dayIn != null && buy != null ? (dayIn / 1000) * buy : null;
  // A generation point's day = the sun's value (self-use at the day's average price + its export
  // revenue) — the same figure as «Солнце сегодня» and Economics (rerun 16.10 GEN-R01).
  const dayMoneyOut = genPoint && dayOut != null
    ? solarValue(dayOut, data.system.totals?.e_day_out, buy, data.system.totals?.rev_day, sell).total
    : dayOut != null && sell != null ? (dayOut / 1000) * sell : null;

  return (
    <Box>
      {/* Back — the page has no menu item (UX audit HEU-22/PNT-21: only the browser's back). */}
      <Button size="small" startIcon={<ArrowBackIcon />} sx={{ mb: 1, ml: -1 }}
        onClick={() => (window.history.length > 1 ? navigate(-1) : navigate('/devices'))}>
        {t('common.back')}
      </Button>
      {/* Header */}
      <Stack direction="row" spacing={2} alignItems="center" sx={{ mb: 2 }}>
        <Box sx={{
          width: 44, height: 44, borderRadius: 1.5, flexShrink: 0,
          border: `1px solid ${ds.border}`, bgcolor: ds.surfaceRaised,
          display: 'flex', alignItems: 'center', justifyContent: 'center',
        }}>
          <svg width="24" height="24" viewBox="0 0 24 24" fill="none"
            stroke={group.present ? ds.inkMuted : ds.statusOffline} strokeWidth="1.75" strokeLinecap="round" strokeLinejoin="round">
            <path d={ICON[iconFor(group)]} />
          </svg>
        </Box>
        <Box sx={{ flex: 1, minWidth: 0 }}>
          <Typography variant="h4" fontWeight={700} noWrap>{fullNameOf(group, unitsCfg)}</Typography>
          <Typography variant="body2" color="text.secondary">
            {roleText} · {srcText} · <Box component="span" sx={{ color: statusColor, fontWeight: 600 }}>{statusText}</Box>
          </Typography>
        </Box>
      </Stack>

      {/* caps as words */}
      <Stack direction="row" spacing={1} flexWrap="wrap" useFlexGap sx={{ mb: 2 }}>
        {capWords.map((w) => (
          <Box key={w} sx={{ px: 1, py: 0.25, borderRadius: 1, border: `1px solid ${ds.border}`, color: ds.inkMuted, fontSize: 12 }}>{w}</Box>
        ))}
      </Stack>

      {/* BASIC with a backwards CT: the load isn't metered at all (forward flow only). */}
      {/* Bound to a channel the fitted module lacks (UI3 in place of UI7): kept on purpose — removing
          it would erase its history; data resumes when a module with that channel is back. */}
      {group.noChannel && (() => {
        const hw = pointHardware(unitKey, hwAll.sensors, hwAll.modules);
        return <Alert severity="info" sx={{ mb: 2 }}>{t('point.noChannel', { ch: channelLabel(hw.sensor?.ch, hw.sensor?.ch_label), model: hw.module?.model ?? '—', n: String(hw.module?.n_i ?? '—') })}</Alert>;
      })()}
      {/* The hub says it (`reversed`, d03f19ed — and drops the sign); older canons: a negative p. */}
      {basicPoint && (typeof group.attributes?.reversed === 'boolean' ? group.attributes.reversed
        : basicReversed(power, hwCtRating, unitMetricValue(group, 'voltage'))) && (
        <Alert severity="error" sx={{ mb: 2 }}>{t('point.basicReversed', { w: pwW(Math.abs(power ?? 0)) })}</Alert>
      )}
      {/* rbamp 1.7.2: the channel's readings aren't corrected for its CT (root 20.10). */}
      {(() => {
        const hs = pointHardware(unitKey, hwAll.sensors, hwAll.modules).sensor;
        return sctUncorrected(hs) ? (
          <Alert severity="warning" sx={{ mb: 2 }}>{t('point.sctUncorrected', { why: tCode('sct', String(hs?.sct_correction)) })}</Alert>
        ) : null;
      })()}
      {/* CT at its limit (d03f19ed): clipping under-counts silently; near the limit = a warning. */}
      <NoHistoryNote history={cfg?.history} historyCount={unitsCfg.historyCount} />
      {group.attributes?.clip === true && <Alert severity="error" sx={{ mb: 2 }}>{t('point.ctClip')}</Alert>}
      {group.attributes?.clip !== true && group.attributes?.ct_warn === true && (
        <Alert severity="warning" sx={{ mb: 2 }}>{t('point.ctWarn', { pct: typeof group.attributes?.load_pct === 'number' ? nf(group.attributes.load_pct as number, 0) : '—' })}</Alert>
      )}
      {/* live values */}
      <Box sx={{ mb: 2 }}>
        <Card>
          <Stack direction="row" spacing={4} flexWrap="wrap" useFlexGap>
            {liveStats.map((s) => (
              <Stack key={s.label} spacing={0.25}>
                <Typography sx={{ fontSize: 12, color: ds.inkMuted }}>{s.label}</Typography>
                <Typography sx={{ ...mono, fontSize: 20, fontWeight: 600, color: s.color ?? ds.ink }}>{s.value}</Typography>
              </Stack>
            ))}
          </Stack>
          {/* Per-phase readings of a U3I3 unit (ph[], root delta №4 — any phase-grouped unit, not
              only the input); this frame only, unknown fields «—». The values above are Σ / mean. */}
          {group.phases && (
            <Box sx={{ mt: 2, pt: 1.5, borderTop: `1px solid ${ds.border}` }}>
              <Typography sx={{ fontSize: 12, color: ds.inkMuted, mb: 0.75 }}>{t('point.byPhase')}</Typography>
              <Box
                sx={{ display: 'grid', gridTemplateColumns: { xs: 'repeat(4, max-content)', sm: 'repeat(5, max-content)' }, justifyContent: 'start', columnGap: { xs: 2, sm: 4 }, rowGap: 0.5, alignItems: 'baseline' }}
              >
                {group.phases.map((r) => {
                  const vb = voltageBand(r.v, grid.nominalV);
                  return (
                    <React.Fragment key={r.ph}>
                      <Typography sx={{ fontWeight: 700, fontSize: 14 }}>L{r.ph}</Typography>
                      <Typography sx={{ ...mono, fontSize: 14, color: vb ? ds.sevAlarm : ds.ink, fontWeight: vb ? 600 : 400 }}>
                        {volt(r.v)}{vb ? ` · ${t(vb === 'low' ? 'cards.voltLow' : 'cards.voltHigh')}` : ''}
                      </Typography>
                      <Typography sx={{ ...mono, fontSize: 14 }}>{curA(r.i)}</Typography>
                      <Typography sx={{ ...mono, fontSize: 14 }}>{pwW(r.p)}</Typography>
                      {/* Q and cos φ share one cell — on a phone it wraps under the row (390 px: 423 px of columns). */}
                      <Typography sx={{ ...mono, fontSize: 14, color: ds.inkMuted, gridColumn: { xs: '2 / -1', sm: 'auto' }, mb: { xs: 0.5, sm: 0 } }}>
                        {reactive(r.q, r.p)} · {r.pf != null ? `${t('units.pf')} ${nf(r.pf, 2)}` : '—'}
                      </Typography>
                    </React.Fragment>
                  );
                })}
              </Box>
            </Box>
          )}
        </Card>
      </Box>

      {/* EV session (stations only) */}
      {station && (
        <Box sx={{ mb: 2 }}>
          <Card>
            <Typography sx={{ fontWeight: 700, fontSize: 15, mb: 1 }}>{t('point.currentSession')}</Typography>
            <Stack direction="row" spacing={4} flexWrap="wrap" useFlexGap>
              <Stack spacing={0.25}>
                <Typography sx={{ fontSize: 12, color: ds.inkMuted }} title={stationStatusTooltip()}>{t('point.status')}</Typography>
                <Typography sx={{ fontSize: 16, fontWeight: 600, color: ds.ink }}>{statusText}</Typography>
              </Stack>
              <Stack spacing={0.25}>
                <Typography sx={{ fontSize: 12, color: ds.inkMuted }}>{t('point.energyToday')}</Typography>
                <Typography sx={{ ...mono, fontSize: 16, fontWeight: 600, color: ds.ink }}>{enWh(dayIn)}</Typography>
              </Stack>
            </Stack>
            <Typography sx={{ fontSize: 12, color: ds.inkMuted, mt: 1 }}>
              {t('point.sessionHistoryNote')}
            </Typography>
          </Card>
        </Box>
      )}

      {/* trend */}
      <Box sx={{ mb: 2 }}>
        <Card>
          <Stack direction="row" alignItems="center" justifyContent="space-between" flexWrap="wrap" useFlexGap sx={{ mb: 1 }}>
            <ToggleButtonGroup size="small" exclusive value={ctype} onChange={(_, v) => v && setCtype(v)}>
              <ToggleButton value="power">{t('point.power')}</ToggleButton>
              <ToggleButton value="voltage">{t('point.voltage')}</ToggleButton>
              <ToggleButton value="energy">{t('point.energy')}</ToggleButton>
            </ToggleButtonGroup>
            <ToggleButtonGroup size="small" exclusive value={period} onChange={(_, v) => v && setPeriod(v)}>
              <ToggleButton value="hour">{t('point.periodHours')}</ToggleButton>
              <ToggleButton value="day">{t('point.periodDays')}</ToggleButton>
              <ToggleButton value="week">{t('point.periodWeeks')}</ToggleButton>
            </ToggleButtonGroup>
            <Link component={RouterLink} to={historyHref({ point: unitKey, win: winForPeriod(period), metric: ctype, view: 'table' })} underline="hover" sx={{ fontSize: 13 }}>{t('common.asTable')}</Link>
          </Stack>
          <Trend
            series={series}
            kind={ctype === 'energy' ? 'bar' : 'line'}
            unit={ctype === 'energy' ? t('units.kWh') : ctype === 'power' ? t('units.W') : t('units.V')}
            color={ds.primary}
            period={period}
            ds={ds}
            nominalV={ctype === 'voltage' ? grid.nominalV : undefined}
          />
        </Card>
      </Box>

      {/* counters — not for a current-only module: it isn't energy metering (operator 18.10) */}
      <Box sx={{ mb: 2, display: currentOnly ? 'none' : undefined }}>
        <Card>
          <Typography sx={{ fontWeight: 700, fontSize: 15, mb: 1 }}>{t('point.counters')}</Typography>
          <Stack direction="row" spacing={4} flexWrap="wrap" useFlexGap>
            <Counter label={t('point.todayIn')} value={enWh(dayIn)} sub={dayMoneyIn != null ? `≈ ${mf.money(dayMoneyIn)}` : undefined} ds={ds} mono={mono} />
            {(dayOut ?? 0) > 0 && <Counter label={t(genPoint ? 'point.todayGen' : 'point.todayOut')} value={enWh(dayOut)} sub={dayMoneyOut != null ? `≈ +${mf.money(dayMoneyOut)}` : undefined} ds={ds} mono={mono} />}
            <Counter label={t('point.totalIn')} value={enWh(totIn)} ds={ds} mono={mono} />
            {/* BASIC counts no export; an «отдано» left from a previous module isn't this point's. */}
            {(totOut ?? 0) > 0 && !basicPoint && <Counter label={t(genPoint ? 'point.totalGen' : 'point.totalOut')} value={enWh(totOut)} ds={ds} mono={mono} />}
          </Stack>
        </Card>
      </Box>

      {/* Viewing comes first; setup is one click away, not a form on the main path (HOME-14). */}
      {/* «…?setup» (the Overview's «Check» on the input limit) opens it expanded and in view (R03). */}
      <Accordion id="setup" defaultExpanded={setupOpen} ref={(el: HTMLDivElement | null) => { if (el && setupOpen && !setupScrolled.current) { setupScrolled.current = true; setTimeout(() => el.scrollIntoView({ block: 'start' }), 300); } }}
        disableGutters sx={{ mb: 2, bgcolor: 'transparent', boxShadow: 'none', '&:before': { display: 'none' }, scrollMarginTop: 16 }}>
        <AccordionSummary expandIcon={<ExpandMoreIcon />} sx={{ px: 0 }}>
          <Typography sx={{ fontWeight: 700, fontSize: 15 }}>{t('point.setupSection')}</Typography>
        </AccordionSummary>
        <AccordionDetails sx={{ px: 0 }}>
      {/* point parameters (name / role / phase / nominal / breaker) */}
      <Box sx={{ mb: 2 }}>
        <UnitParamsPanel apiId={apiId} />
      </Box>

      {/* technical details */}
      <Box sx={{ mb: 2 }}>
        <Card>
          <Typography sx={{ fontWeight: 700, fontSize: 15, mb: 1 }}>{t('point.techDetails')}</Typography>
          <Box sx={{ ...mono, fontSize: 12, color: ds.inkMuted, whiteSpace: 'pre-wrap', lineHeight: 1.8 }}>
            {[
              `unit_id   ${group.unitKey}`,
              `point     ${group.point ?? '—'}`,
              `source    ${group.src ?? '—'} · ${group.srcClass}`,
              `module/ch ${String(group.attributes?.module ?? '—')} / ${chText(group.attributes?.ch, group.attributes?.ch_label)}`,
              `caps      ${(group.caps ?? []).join(', ') || '—'}`,
              cfg?.ratedPowerW ? t('point.techRated', { value: `${nf(cfg.ratedPowerW)} ${t('units.W')}` }) : '',
            ].filter(Boolean).join('\n')}
          </Box>
        </Card>
      </Box>

      {/* delete — last, sev-critical */}
      <Box>
        {delErr && <Alert severity="error" onClose={() => setDelErr(null)} sx={{ mb: 1 }}>{delErr}</Alert>}
        {deleteLocked ? (
          <Typography sx={{ fontSize: 13, color: ds.inkMuted }}>{t('point.deleteLocked')}</Typography>
        ) : (
          <Button onClick={deletePoint} disabled={delBusy || !apiId} sx={{ color: ds.sevCritical, textTransform: 'none' }}>
            {t('point.deletePoint')}
          </Button>
        )}
      </Box>
        </AccordionDetails>
      </Accordion>
    </Box>
  );
};


const Counter: React.FC<{ label: string; value: string; sub?: string; ds: ReturnType<typeof useDsTokens>; mono: object }> = ({ label, value, sub, ds, mono }) => (
  <Stack spacing={0.25}>
    <Typography sx={{ fontSize: 12, color: ds.inkMuted }}>{label}</Typography>
    <Typography sx={{ ...mono, fontSize: 18, fontWeight: 600, color: ds.ink }}>{value}</Typography>
    {sub && <Typography sx={{ ...mono, fontSize: 12, color: ds.inkMuted }}>{sub}</Typography>}
  </Stack>
);

/**
 * Trend chart at a fixed 240 px height, drawn in REAL pixels for the measured card width
 * (`viewBox = 0 0 W H`) so the 11 px labels never scale (review 03 P0-8). X labels thin out
 * to keep ≥ 36 px apart.
 */
const TREND_H = 240;
const Trend: React.FC<{
  series: { ts: number; value: number }[];
  kind: 'bar' | 'line';
  unit: string;
  color: string;
  period: HistoryPeriod;
  ds: ReturnType<typeof useDsTokens>;
  /** Voltage: the axis holds the ±10 % limits around this nominal (charts audit L2). */
  nominalV?: number;
}> = ({ series, kind, unit, color, period, ds, nominalV }) => {
  const [measureRef, measured] = useElementWidth<HTMLDivElement>(760);
  const clock = useDeviceClock();
  const W = Math.max(240, measured), H = TREND_H, PAD_L = 8, PAD_R = 8, PAD_T = 14, PAD_B = 22;
  if (series.length === 0) {
    return <Box sx={{ py: 4, textAlign: 'center', color: ds.inkMuted, fontSize: 13 }}>{t('point.noDataForPeriod')}</Box>;
  }
  const plotW = W - PAD_L - PAD_R, plotH = H - PAD_T - PAD_B;
  const vals = series.map((s) => s.value);
  // Power and energy from 0; voltage in a window holding the ±10 % limits — a min–max axis made
  // a 2 % wobble look like an emergency (charts audit L2).
  let max = Math.max(0.001, ...vals);
  let min = kind === 'line' ? Math.min(0, ...vals) : 0;
  if (nominalV) { min = Math.min(nominalV * 0.88, ...vals); max = Math.max(nominalV * 1.12, ...vals); }
  const span = Math.max(0.001, max - min);
  const n = series.length;
  // Lines by TIME, not by index: a gap or a restart fragment no longer squeezes the hours.
  const tsMin = series[0].ts, stepSec = period === 'hour' ? 3600 : period === 'day' ? 86400 : 7 * 86400;
  const lineCurve: Curve = period === 'hour' && !nominalV ? 'step' : 'monotoneX';
  const tsMax = series[n - 1].ts + (lineCurve === 'step' ? stepSec : 0);
  const xT = (ts: number) => PAD_L + (tsMax === tsMin ? plotW / 2 : ((ts - tsMin) / (tsMax - tsMin)) * plotW);
  const x = (i: number) => (kind === 'line' ? xT(series[i].ts) : PAD_L + (n === 1 ? plotW / 2 : (plotW * i) / (n - 1)));
  const y = (v: number) => PAD_T + plotH - ((v - min) / span) * plotH;
  const multiDay = series.length > 1 && clock.dayStart(series[n - 1].ts) !== clock.dayStart(series[0].ts);
  const fmtTick = (ts: number) => {
    // The hub's clock (W1-8), not the browser's. Hours across days carry the date at each new
    // day (H3P-13: «22 23 00 01» gave no clue the day had changed).
    if (period === 'hour') {
      const p = clock.parts(ts);
      if (multiDay && p.h === 0) return formatDeviceTime(clock, ts, { day: '2-digit', month: '2-digit' }, dateLocale());
      return `${p.h}`.padStart(2, '0');
    }
    return formatDeviceTime(clock, ts, { day: '2-digit', month: '2-digit' }, dateLocale());
  };
  const minGap = period === 'hour' ? 32 : 48;
  const step = n <= 1 ? 1 : Math.max(1, Math.ceil(minGap / (plotW / (n - 1))));

  return (
    // svg absolutely positioned: its pixel width must not hold the card open when it shrinks.
    <Box ref={measureRef} sx={{ position: 'relative', width: '100%', minWidth: 0, height: H, overflow: 'hidden',
      '& > svg': { display: 'block', position: 'absolute', left: 0, top: 0 } }}>
      <svg viewBox={`0 0 ${W} ${H}`} width={W} height={H} role="img" aria-label={t('point.trendAria', { unit })}>
        {/* gridlines */}
        {[0, 0.5, 1].map((f) => {
          const gy = PAD_T + plotH * (1 - f);
          return <line key={f} x1={PAD_L} x2={W - PAD_R} y1={gy} y2={gy} stroke={ds.chartGrid} strokeWidth="1" />;
        })}
        {kind === 'bar' ? (
          series.map((s, i) => {
            const slot = plotW / Math.max(1, n);
            const bw = Math.min(26, slot * 0.6);
            const cx = PAD_L + slot * i + slot / 2;
            const h = (s.value / max) * plotH;
            return (
              <rect key={i} x={cx - bw / 2} y={PAD_T + plotH - h} width={bw} height={Math.max(1, h)} rx="2" fill={color} opacity="0.9">
                <title>{`${fmtTick(s.ts)}${period === 'hour' ? ':00' : ''} — ${nf(s.value, 2)} ${unit}`}</title>
              </rect>
            );
          })
        ) : (
          // One record = a dot with its value, not an empty frame (INP-R14).
          n === 1 ? (
            <g>
              <circle cx={x(0)} cy={y(series[0].value)} r={4} fill={color} />
              <text x={x(0)} y={y(series[0].value) - 8} textAnchor="middle" fill={ds.ink} fontSize="12">{`${scaleNum(series[0].value)} ${unit}`}</text>
            </g>
          ) : (
            <g>
              {nominalV && [nominalV * 0.9, nominalV * 1.1].map((v) => (
                <line key={v} x1={PAD_L} x2={W - PAD_R} y1={y(v)} y2={y(v)} stroke={ds.sevWarning} strokeWidth={1} strokeDasharray="6 4" opacity={0.7} />
              ))}
              {runsOf(series, (s) => s.ts, (s) => s.value, stepSec * 1.5).map((rs, ri) => (
                <path key={ri} fill="none" stroke={color} strokeWidth="2" strokeLinejoin="round" strokeLinecap="round"
                  d={linePath(rs.map((s) => ({ x: xT(s.ts), y: y(s.value) })), lineCurve, xT(rs[rs.length - 1].ts + stepSec))} />
              ))}
            </g>
          )
        )}
        {/* one hover card for mouse, touch and keys (charts spec 17.10) */}
        <HoverLayer
          ts={series.map((p) => p.ts)}
          xOf={(ts) => {
            const i = series.findIndex((p) => p.ts === ts);
            if (kind === 'line') return lineCurve === 'step' ? xT(ts + stepSec / 2) : xT(ts);
            const slot = plotW / Math.max(1, n);
            return PAD_L + slot * i + slot / 2;
          }}
          title={(ts) => (period === 'hour'
            ? `${formatDeviceTime(clock, ts, { day: '2-digit', month: '2-digit' }, dateLocale())} ${String(clock.parts(ts).h).padStart(2, '0')}:00`
            : formatDeviceTime(clock, ts, { day: '2-digit', month: '2-digit' }, dateLocale()))}
          rows={(ts) => { const p = series.find((q) => q.ts === ts); return p ? [{ label: '', value: `${scaleNum(p.value)} ${unit}`, color }] : []; }}
          left={PAD_L} right={W - PAD_R} top={PAD_T} bottom={PAD_T + plotH}
          ink={ds.ink} muted={ds.inkMuted} surface={ds.surface} border={ds.border} ariaLabel={t('point.trendAria', { unit })} />
        {/* the scale: top and bottom values (INP-10: no y axis at all) */}
        <text x={PAD_L} y={PAD_T - 3} fill={ds.inkMuted} fontSize="10">{`${scaleNum(max)} ${unit}`}</text>
        {kind === 'line' && <text x={PAD_L} y={PAD_T + plotH - 3} fill={ds.inkMuted} fontSize="10">{`${scaleNum(min)} ${unit}`}</text>}
        {/* Each label once — fragments of one hour after a restart all read «23» (INP-10). */}
        {series.map((s, i) => {
          const lbl = fmtTick(s.ts);
          const prevShown = series.slice(0, i).reverse().find((_, k) => (i - 1 - k) % step === 0);
          // A new day's label always shows (H3P-13); a regular one too close to it gives way.
          const dayMark = multiDay && period === 'hour' && clock.parts(s.ts).h === 0;
          const nearMark = !dayMark && multiDay && period === 'hour' && series.some((o) => clock.parts(o.ts).h === 0 && Math.abs(x(series.indexOf(o)) - x(i)) < minGap);
          if (!dayMark && (nearMark || i % step !== 0 || (prevShown && fmtTick(prevShown.ts) === lbl))) return null;
          return (
            <text key={`t${i}`} x={x(i)} y={H - 6} fill={ds.inkMuted} fontSize="11"
              textAnchor={i === 0 ? 'start' : i === n - 1 ? 'end' : 'middle'}>{lbl}</text>
          );
        })}
      </svg>
    </Box>
  );
};
