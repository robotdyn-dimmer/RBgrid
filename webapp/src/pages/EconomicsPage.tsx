/**
 * EconomicsPage — «Экономика»: ключевая биллинговая поверхность. Расход/доход по точке
 * и зонам, банк кредита взаимозачёта, прогрессивный тариф и оконные итоги — из
 * /api/v1/economics (накопительно) + /api/v1/summary (оконные итоги; по зонам накопительно).
 * Деньги — в валюте тарифа, энергия — в кВт·ч. Шов «накопительно vs за период» подписан,
 * чтобы цифры не путали. Оформление — на дизайн-системе, значения — ru-RU.
 */
import { useTariffPlan } from '../hooks/useTariffPlan';
import { useDsTokens } from '../theme/useDsTokens';
import { StackedBar, zoneColorOf } from '../components/charts/StackedBar';
import React, { useEffect, useMemo, useState } from 'react';
import { unitMetricValue } from '../entities/model';
import type { WindowTotals } from './historyWindow';
import { useTelemetryEntities } from '../hooks/useTelemetryEntities';
import { HelpTip } from '../components/common/HelpTip';
import { Box, Typography, Grid, Stack, Alert, CircularProgress } from '@mui/material';
import { Card, FormField, StatTile, InfoField, SettingRow, StatusChip } from '@rbgrid/design-system';
import { useSensors } from '../hooks/useSensors';
import { useEconomics, useConfigShadow } from '../core';
import type { JsonObject } from '../core';
import { enWh, nf } from '../components/common/ruFormat';
import { useMoneyFormat } from '../hooks/useMoneyFormat';
import { t, tCode } from '../i18n';
import { useUnitsConfig } from '../hooks/useUnitsConfig';
import { usePeriodTotals, PeriodTotals, TotalsPeriod, isPastPeriod } from '../hooks/usePeriodTotals';
import { UnitEntityGroup } from '../entities/model';
import { roleLabel, mainUnit } from './overviewData';
import { fullDisplayName, isGenerationRole, hexKey, apiUnitId } from '../entities/siteModel';
import { useInputSeries } from '../hooks/useInputSeries';
import { useHardware } from '../hooks/useHardware';
import { isBasic, pointHardware } from '../entities/moduleLine';

const PERIODS: TotalsPeriod[] = ['hour', 'day', 'yesterday', 'week', 'month', 'lastMonth', 'year'];

/** Период — существительное (для выпадающего списка). */
const PERIOD_LABEL: Record<TotalsPeriod, string> = {
  yesterday: 'economics.periodYesterday', lastMonth: 'economics.periodLastMonth',
  hour: 'economics.periodHour', day: 'economics.periodDay', week: 'economics.periodWeek',
  month: 'economics.periodMonth', year: 'economics.periodYear',
};
/** Период — «за …» (для подписи-плашки итогов). */
const PERIOD_ACC: Record<TotalsPeriod, string> = {
  yesterday: 'economics.forYesterday', lastMonth: 'economics.forLastMonth',
  hour: 'economics.forHour', day: 'economics.forDay', week: 'economics.forWeek',
  month: 'economics.forMonth', year: 'economics.forYear',
};

/** Русские подписи тарифных зон; незнакомые — гуманизируем. */
const ZONE_LABEL_RU: Record<string, string> = {
  peak: 'economics.zonePeak', off_peak: 'economics.zoneOffPeak', day: 'economics.zoneDay', night: 'economics.zoneNight',
  shoulder: 'economics.zoneShoulder', standard: 'economics.zoneStandard',
};
const zoneLabel = (z: string): string => {
  if (ZONE_LABEL_RU[z]) return t(ZONE_LABEL_RU[z]);
  const code = tCode('zone', z.toUpperCase());
  return code !== z.toUpperCase() ? code : z.replace(/_/g, ' ').replace(/^\w/, (c) => c.toUpperCase());
};

/** Энергия: контракт отдаёт кВт·ч, а enWh() ждёт Вт·ч → переводим. «—» для отсутствия. */
const en = (kwh: number | null | undefined): string => enWh(kwh == null ? null : kwh * 1000);

export const EconomicsPage: React.FC = () => {
  const ds = useDsTokens();
  const { sensors } = useSensors();
  const unitsCfg = useUnitsConfig();
  const cfgOf = (key: string) => unitsCfg.forGroup({ unitKey: key } as UnitEntityGroup);
  // Points with energy only (the voltage bus has none — on .110 it was the default and read all
  // zeros, UX audit HEU-09/RES-03); the main input first, then by name.
  const unitsShadow = useConfigShadow('units');
  const units = useMemo(() => {
    // Bound sensors + every configured point (ESP-NOW stations have no sensor entry — the demo's
    // ЗС-1/ЗС-2 were missing from the point list and the balance).
    const cfgIds = (Array.isArray(unitsShadow.reported.units) ? (unitsShadow.reported.units as JsonObject[]) : [])
      .map((u) => hexKey(u.unit_id)).filter((k): k is string => !!k);
    const ids = Array.from(new Set([...sensors.filter((s) => s.unit_id).map((s) => (s.unit_id as string).toUpperCase()), ...cfgIds]))
      .filter((id) => cfgOf(id)?.type !== 'VOLTAGE_BUS');
    const rank = (id: string) => (cfgOf(id)?.role === 'MAIN_SUPPLY' ? 0 : 1);
    return ids.sort((a, b) => rank(a) - rank(b));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [sensors, unitsCfg, unitsShadow.reported]);
  const pointLabel = (id: string): string => {
    const c = cfgOf(id);
    const name = fullDisplayName({ unitKey: id } as UnitEntityGroup, c);
    const role = c?.role ? roleLabel(c.role) : '';
    return role && role !== name ? `${name} · ${role}` : name;
  };
  const [unitId, setUnitId] = useState<string | null>(null);
  // A BASIC point counts consumption only (root dd861a54): no export, revenue or net figures.
  const hardware = useHardware();
  const basicUnit = !!unitId && pointHardware(unitId, hardware.sensors, hardware.modules).basic;
  const [period, setPeriod] = useState<TotalsPeriod>('day');

  const telem = useTelemetryEntities();
  // The input = the hub's main point: on a per-phase input its L2/L3 points are MAIN_SUPPLY too.
  const hubMain = telem.data ? mainUnit(telem.data.units, unitsCfg) : undefined;
  const hubMainHex = hubMain ? (unitsCfg.forGroup(hubMain)?.hex ?? apiUnitId(hubMain.unitKey))?.toUpperCase() ?? null : null;
  const mainId = (hubMainHex && units.includes(hubMainHex) ? hubMainHex : null)
    ?? units.find((id) => cfgOf(id)?.role === 'MAIN_SUPPLY') ?? null;
  // Its history/money as one series — all phases summed where the hub can (hooks/useInputSeries).
  const inputSeries = useInputSeries(mainId);
  const seriesOf = (id: string | null) => (id != null && id === mainId ? inputSeries : id);
  // Default = the main input; re-pick once units-config arrives if the first guess was a bus.
  useEffect(() => { if ((!unitId || !units.includes(unitId)) && units.length) setUnitId(mainId ?? units[0]); }, [units, unitId, mainId]);
  const eco = useEconomics(seriesOf(unitId));
  // The net-metering credit means nothing while the plan has net metering off; the progressive block
  // has no setting in Tariffs — shown only when the hub runs one (docs review 04.11).
  const { plan: tariffPlan } = useTariffPlan();
  const netMeteringOn = tariffPlan?.net_metering_enabled !== false;
  // The period from the hub's closed records by its clock (hooks/usePeriodTotals): the hub's
  // /summary without from/to sums its whole ring, and a running month has no record yet.
  // «Сегодня» of the INPUT = the hub's day counters (the Overview's figure); other points and
  // the hours before today come from closed records.
  const sysT = telem.data?.system.totals;
  const todayLive = useMemo(() => (sysT && unitId != null && unitId === mainId && sysT.e_day_in != null
    ? { inWh: Math.round(sysT.e_day_in ?? 0), outWh: Math.round(sysT.e_day_out ?? 0), cost: Math.round((sysT.cost_day ?? 0) * 100) / 100, revenue: Math.round((sysT.rev_day ?? 0) * 100) / 100, n: 1 }
    : null), [Math.round(sysT?.e_day_in ?? -1), Math.round(sysT?.e_day_out ?? -1), sysT?.cost_day, sysT?.rev_day, unitId, mainId]); // eslint-disable-line react-hooks/exhaustive-deps
  const totals = usePeriodTotals(seriesOf(unitId), period, { today: period === 'hour' || isPastPeriod(period) ? null : todayLive });
  // Generation points (RENEWABLE_SOURCE) — the «Генерация» section (UX audit W2-4).
  const genIds = useMemo(() => units.filter((id) => isGenerationRole(cfgOf(id)?.role)),
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [units, unitsCfg]);
  const mainBasic = !!mainId && pointHardware(mainId, hardware.sensors, hardware.modules).basic;
  // Metered lines (not the input, the voltage bus or a source) — the balance's «lines» side.
  const lineIds = useMemo(() => units.filter((id) => {
    const c = cfgOf(id);
    return c?.role !== 'MAIN_SUPPLY' && c?.role !== 'ALTERNATIVE_SUPPLY' && c?.type !== 'VOLTAGE_BUS' && !isGenerationRole(c?.role);
  }),
  // eslint-disable-next-line react-hooks/exhaustive-deps
  [units, unitsCfg]);
  // Currency/precision from the device (response ISO code → schema.money) — never hardcoded.
  const mf = useMoneyFormat(eco.data?.currency);

  return (
    <Box>
      <Typography variant="h4" fontWeight={800} sx={{ mb: 2.5 }}>{t('economics.title')}</Typography>

      <Grid container spacing={2} sx={{ mb: 2 }}>
        <Grid item xs={12} sm={6}>
          <FormField
            label={t('economics.point')} value={unitId ?? ''} onChange={setUnitId}
            options={units.length ? units.map((u) => ({ value: u, label: pointLabel(u) })) : [{ value: '', label: t('economics.noPoints') }]}
          />
        </Grid>
        <Grid item xs={6} sm={3}>
          <FormField
            label={t('economics.period')} value={period} onChange={(v) => setPeriod(v as TotalsPeriod)}
            options={PERIODS.map((p) => ({ value: p, label: t(PERIOD_LABEL[p]) }))}
          />
        </Grid>
      </Grid>

      {/* Оконные итоги */}
      <Box sx={{ mb: 2 }}>
        <Card>
          <Stack direction="row" alignItems="center" spacing={1.5} mb={1.5}>
            <Typography variant="h6" fontWeight={700}>{t('economics.totals')}</Typography>
            <StatusChip status="present" label={t(PERIOD_ACC[period])} />
          </Stack>
          {totals?.loading && !totals.n && <Box sx={{ display: 'flex', justifyContent: 'center', py: 3 }}><CircularProgress size={22} aria-label={t('common.loading')} /></Box>}
          {!totals?.loading && !totals?.n && <Alert severity="info">{unitId ? t('economics.noDataForPoint') : t('economics.selectPoint')}</Alert>}
          {!!totals?.n && (
            <>
              <Grid container spacing={2}>
                <Grid item xs={6} sm={3}><StatTile label={t('economics.imported')} value={enWh(totals.inWh)} emphasis /></Grid>
                {!basicUnit && <Grid item xs={6} sm={3}><StatTile label={t('economics.exported')} value={enWh(totals.outWh)} subColor="success.main" /></Grid>}
                <Grid item xs={6} sm={3}><StatTile label={t('economics.cost')} value={mf.money(totals.cost)} /></Grid>
                {!basicUnit && <Grid item xs={6} sm={3}><StatTile label={t('economics.revenue')} value={mf.money(totals.revenue)} subColor="success.main" /></Grid>}
                {!basicUnit && <Grid item xs={6} sm={3}><StatTile label={t('economics.netEnergy')} value={enWh(totals.inWh - totals.outWh)} /></Grid>}
                {!basicUnit && <Grid item xs={6} sm={3}><StatTile label={t('economics.netCost')} value={mf.money(totals.cost - totals.revenue)} emphasis /></Grid>}
              </Grid>
              {basicUnit && <Typography variant="caption" color="text.secondary" sx={{ display: 'block', mt: 1.5 }}>{t('economics.basicNoExport')}</Typography>}
              <Typography variant="caption" color="text.secondary" sx={{ display: 'block', mt: 1.5 }}>{isPastPeriod(period) ? t('economics.closedPast') : todayLive && period !== 'hour' ? t('economics.todayByCounters') : t('economics.closedOnly')}<HelpTip topic="closedRecords" /></Typography>
            </>
          )}
        </Card>
      </Box>

      {genIds.length > 0 && mainId && (
        <GenerationSection genIds={genIds} mainId={inputSeries ?? mainId} period={period} mf={mf} todayLive={todayLive} />
      )}
      {lineIds.length > 0 && mainId && (
        <BalanceSection lineIds={lineIds} mainId={inputSeries ?? mainId} period={period} todayLive={todayLive} nameOf={pointLabel} />
      )}

      {/* Накопительное состояние биллинга */}
      {eco.loading && <Box sx={{ display: 'flex', justifyContent: 'center', py: 4 }}><CircularProgress aria-label={t('common.loading')} /></Box>}
      {!eco.loading && eco.data && (
        <Grid container spacing={2}>
          <Grid item xs={12} md={6}>
            <Card outlined>
              <Typography variant="overline" color="text.secondary">{t('economics.byZones')}</Typography>
              {/* One bar of the zones' cost — where the money went at a glance (charts audit K) — the rows
                  below keep the exact figures and any revenue. */}
              <Box sx={{ mt: 1, mb: 1.5 }}>
                <StackedBar fmt={(v) => mf.money(v)} total={mf.money(eco.data.zones.reduce((a, z) => a + z.cost, 0))}
                  parts={eco.data.zones.filter((z) => z.cost > 0).map((z) => ({ key: z.zone, label: zoneLabel(z.zone), value: z.cost, color: zoneColorOf(z.zone, ds.series) }))} />
              </Box>
              <Stack spacing={0.5} sx={{ mt: 1 }}>
                {/* Only zones with charges in the period — the hub reports all 8 fixed slots (TAR-09). */}
                {eco.data.zones.filter((z) => z.cost > 0 || z.revenue > 0).map((z) => (
                  <SettingRow key={z.zone} label={zoneLabel(z.zone)}
                    value={z.revenue > 0 ? t('economics.zoneCostRevenue', { cost: mf.money(z.cost), revenue: mf.money(z.revenue) }) : mf.money(z.cost)} />
                ))}
                {!eco.data.zones.some((z) => z.cost > 0 || z.revenue > 0) && (
                  <Typography variant="body2" color="text.secondary">{t('economics.noZoneCharges')}</Typography>
                )}
              </Stack>
            </Card>
          </Grid>
          <Grid item xs={12} md={6}>
            <Stack spacing={2}>
              {netMeteringOn && <Card outlined>
                <Typography variant="overline" color="text.secondary">{t('economics.netMetering')}</Typography>
                {mainBasic && <Typography variant="body2" color="text.secondary" sx={{ mt: 0.5 }}>{t('economics.basicNoNetMetering')}</Typography>}
                <Stack direction="row" spacing={3} flexWrap="wrap" useFlexGap sx={{ mt: 1 }}>
                  <InfoField label={t('economics.creditBalance')} value={en(eco.data.net_metering.credit_balance_kwh)} />
                  <InfoField label={t('economics.periodImport')} value={en(eco.data.net_metering.period_import_kwh)} />
                  <InfoField label={t('economics.periodExport')} value={en(eco.data.net_metering.period_export_kwh)} />
                  <InfoField label={t('economics.lifetimeEarned')} value={en(eco.data.net_metering.lifetime_earned_kwh)} />
                  <InfoField label={t('economics.lifetimeUsed')} value={en(eco.data.net_metering.lifetime_used_kwh)} />
                </Stack>
              </Card>}
              {eco.data.progressive.enabled && <Card outlined>
                <Stack direction="row" alignItems="center" spacing={1.5} mb={1}>
                  <Typography variant="overline" color="text.secondary">{t('economics.progressive')}</Typography>
                  <StatusChip status={eco.data.progressive.enabled ? 'live' : 'dormant'} label={eco.data.progressive.enabled ? t('economics.on') : t('economics.off')} />
                </Stack>
                {eco.data.progressive.enabled && (
                  <Stack direction="row" spacing={3} flexWrap="wrap" useFlexGap>
                    <InfoField label={t('economics.tier')} value={String(eco.data.progressive.tier)} />
                    <InfoField label={t('economics.cumulative')} value={en(eco.data.progressive.cumulative_import_kwh)} />
                    <InfoField label={t('economics.multiplier')} value={nf(eco.data.progressive.rate_mult, 2)} />
                  </Stack>
                )}
              </Card>}
            </Stack>
          </Grid>
        </Grid>
      )}
    </Box>
  );
};

/** One generation point's period totals, reported up (hooks can't run in a loop). */
const GenProbe: React.FC<{ id: string; period: TotalsPeriod; today: WindowTotals | null; onTotals: (id: string, v: PeriodTotals | null) => void }> = ({ id, period, today, onTotals }) => {
  // Today = the point's own day counter (like the input's), earlier = closed records.
  const v = usePeriodTotals(id, period, { money: false, today: period === 'hour' || isPastPeriod(period) ? null : today });
  useEffect(() => { onTotals(id, v); }, [id, v, onTotals]);
  return null;
};

/**
 * «Генерация» for the period (UX audit W2-4 / GEN-05): produced = the generation points' output;
 * what left through the main input was sold (its revenue), the rest the site used itself and did
 * not buy — valued at the period's average buying price of the input (≈).
 */
const GenerationSection: React.FC<{ genIds: string[]; mainId: string; period: TotalsPeriod; mf: ReturnType<typeof useMoneyFormat>; todayLive: WindowTotals | null }> = ({ genIds, mainId, period, mf, todayLive }) => {
  const [gen, setGen] = useState<Record<string, PeriodTotals | null>>({});
  const onTotals = React.useCallback((id: string, v: PeriodTotals | null) => setGen((g) => (g[id] === v ? g : { ...g, [id]: v })), []);
  const input = usePeriodTotals(mainId, period, { today: period === 'hour' || isPastPeriod(period) ? null : todayLive });
  const telem = useTelemetryEntities();
  // Rounded to 1 Wh and memoised by value — a new object per frame would re-run the probes.
  const genKey = genIds.map((id) => {
    const g = telem.data?.units.find((u) => u.unitKey.toUpperCase() === id.toUpperCase());
    const out = g ? unitMetricValue(g, 'energy_day_out') : null;
    return `${id}:${out == null ? '' : Math.round(out)}`;
  }).join(',');
  const genTodayMap = useMemo(() => new Map(genKey.split(',').map((kv) => {
    const [id, out] = kv.split(':');
    return [id, out === '' || out === undefined ? null : { inWh: 0, outWh: Number(out), cost: 0, revenue: 0, n: 1 }] as const;
  })), [genKey]);
  const genToday = (id: string): WindowTotals | null => genTodayMap.get(id) ?? null;
  const produced = genIds.reduce((s, id) => s + (gen[id]?.outWh ?? 0), 0);
  const known = genIds.some((id) => (gen[id]?.n ?? 0) > 0);
  const sold = Math.min(produced, input?.outWh ?? 0);
  const self = Math.max(0, produced - sold);
  const avgBuy = input && input.inWh > 0 && input.cost > 0 ? input.cost / (input.inWh / 1000) : null;
  const saved = avgBuy != null ? (self / 1000) * avgBuy : null;
  return (
    <Box sx={{ mb: 2 }}>
      {genIds.map((id) => <GenProbe key={id} id={id} period={period} today={genToday(id)} onTotals={onTotals} />)}
      <Card>
        <Typography variant="h6" fontWeight={700} sx={{ mb: 1.5 }}>{t('economics.genTitle')}</Typography>
        {!known ? (
          <Typography variant="body2" color="text.secondary">{t('economics.genNoData')}</Typography>
        ) : (
          <>
            <Grid container spacing={2}>
              <Grid item xs={6} sm={3}><StatTile label={t('economics.genProduced')} value={enWh(produced)} emphasis /></Grid>
              <Grid item xs={6} sm={3}><StatTile label={t('economics.genSelf')} value={enWh(self)} /></Grid>
              <Grid item xs={6} sm={3}><StatTile label={t('economics.genSaved')} value={saved != null ? `≈ ${mf.money(saved)}` : '—'} subColor="success.main" /></Grid>
              <Grid item xs={6} sm={3}><StatTile label={t('economics.genSold')} value={`${enWh(sold)} · ${mf.money(input?.revenue ?? 0)}`} subColor="success.main" /></Grid>
              {/* The two shares HA's Energy dashboard shows (HA-R08): how much of the sun the site used
                  itself, and how much of the site's need the sun covered. */}
              <Grid item xs={6} sm={3}><StatTile label={t('economics.genSelfShare')} value={produced > 0 ? `${nf((self / produced) * 100)} %` : '—'} /></Grid>
              <Grid item xs={6} sm={3}><StatTile label={t('economics.genAutarky')} value={self + (input?.inWh ?? 0) > 0 ? `${nf((self / (self + (input?.inWh ?? 0))) * 100)} %` : '—'} /></Grid>
            </Grid>
            <Typography variant="caption" color="text.secondary" sx={{ display: 'block', mt: 1.5 }}>
              {avgBuy != null ? t('economics.genExplain', { rate: mf.rate(avgBuy) }) : t('economics.genExplainNoRate')}
              <HelpTip topic="notBought" />
            </Typography>
          </>
        )}
      </Card>
    </Box>
  );
};

/** Today = the live day counter of a point (rounded, memoised by value), for a period probe. */
function useLiveToday(ids: string[], field: 'energy_day_in' | 'energy_day_out'): (id: string) => WindowTotals | null {
  const telem = useTelemetryEntities();
  const key = ids.map((id) => {
    const g = telem.data?.units.find((u) => u.unitKey.toUpperCase() === id.toUpperCase());
    const v = g && g.present ? unitMetricValue(g, field) : null;
    return `${id}:${v == null ? '' : Math.round(v)}`;
  }).join(',');
  const map = useMemo(() => new Map(key.split(',').map((kv) => {
    const [id, v] = kv.split(':');
    const wh = v === '' || v === undefined ? null : Number(v);
    return [id, wh == null ? null : { inWh: field === 'energy_day_in' ? wh : 0, outWh: field === 'energy_day_out' ? wh : 0, cost: 0, revenue: 0, n: 1 }] as const;
  })), [key, field]);
  return (id: string) => map.get(id) ?? null;
}

/**
 * «Баланс» for the period (rerun 16.10 SCADA-R11): the input against the sum of its metered
 * lines — the unmetered remainder in kWh and %. A line with no records for the period (offline,
 * new) makes the balance INCOMPLETE and says which (never «everything is unmetered»).
 */
// Below this the input reads ~0 kWh (a bench input, a solar house at noon): a share of it means nothing.
const BALANCE_PCT_FLOOR_WH = 10;

const BalanceSection: React.FC<{ lineIds: string[]; mainId: string; period: TotalsPeriod; todayLive: WindowTotals | null; nameOf: (id: string) => string }> = ({ lineIds, mainId, period, todayLive, nameOf }) => {
  const [lines, setLines] = useState<Record<string, PeriodTotals | null>>({});
  const onTotals = React.useCallback((id: string, v: PeriodTotals | null) => setLines((g) => (g[id] === v ? g : { ...g, [id]: v })), []);
  const input = usePeriodTotals(mainId, period, { money: false, today: period === 'hour' || isPastPeriod(period) ? null : todayLive });
  const lineToday = useLiveToday(lineIds, 'energy_day_in');
  const sum = lineIds.reduce((s, id) => s + (lines[id]?.inWh ?? 0), 0);
  const missing = lineIds.filter((id) => !lines[id] || (lines[id]?.n ?? 0) === 0);
  const inWh = input?.inWh ?? 0;
  const rest = inWh - sum;
  return (
    <Box sx={{ mb: 2 }}>
      {lineIds.map((id) => <GenProbe key={id} id={id} period={period} today={lineToday(id)} onTotals={onTotals} />)}
      <Card>
        <Typography variant="h6" fontWeight={700} sx={{ mb: 1.5 }}>{t('economics.balanceTitle')}</Typography>
        {!input?.n ? (
          <Typography variant="body2" color="text.secondary">{t('economics.noDataForPoint')}</Typography>
        ) : (
          <>
            <Grid container spacing={2}>
              <Grid item xs={6} sm={3}><StatTile label={t('economics.balanceInput')} value={enWh(inWh)} emphasis /></Grid>
              <Grid item xs={6} sm={3}><StatTile label={t('economics.balanceLines', { n: lineIds.length - missing.length })} value={enWh(sum)} /></Grid>
              <Grid item xs={6} sm={3}><StatTile label={t('economics.balanceRest')} value={missing.length ? '—' : enWh(rest)} /></Grid>
              <Grid item xs={6} sm={3}><StatTile label={t('economics.balanceRestPct')} value={missing.length || inWh < BALANCE_PCT_FLOOR_WH ? '—' : `${nf((rest / inWh) * 100)} %`} /></Grid>
            </Grid>
            {missing.length > 0 && (
              <Alert severity="info" sx={{ mt: 1.5 }}>{t('economics.balanceIncomplete', { points: missing.map(nameOf).join(', ') })}</Alert>
            )}
          </>
        )}
      </Card>
    </Box>
  );
};
