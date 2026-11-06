/**
 * DashboardPage — the Overview (spec §4), SCENARIO-driven by site_profile (§3) with a shared
 * «Аналитика» view (§4.4). «Одно число — одно место» (review 04 §5): every scenario opens with
 * ITS primary answer (on phones too — one column in the same order), secondary blocks below.
 *  - home (§4.1): PowerHero → Today (taken · given · net + tariff) → Экономия → HomeFlowCard
 *    (Схема | Сцена, group tabs) → hourly → point tiles (lines only:
 *    the main input, v_bus/alt and other supply/pseudo units are already the hero);
 *  - industrial (§4.2): InputLimitHero (ввод против лимита + неучтённое) → phases →
 *    lines table + «Требует внимания» → tree. No tiles;
 *  - ev (§4.3): EvOverview (ParkingHero → KPI → stations → parking power vs limit → tree);
 *  - Аналитика (§4.4): KPI with data only → hourly + alarms/tariff → flow → points table.
 *
 * Shared states: loading, empty (→ wizard), no-connection (stale banner), seq-gap hint.
 */
import React, { useMemo, useRef, useState } from 'react';
import { SiteProfilePanel } from '../components/settings/SiteProfilePanel';
import { usePeriodTotals } from '../hooks/usePeriodTotals';
import { useInputSeries } from '../hooks/useInputSeries';
import { effectiveInputLimitW } from '../entities/gridSystem';
import { Box, Typography, Grid, CircularProgress, Alert, Button, Chip, Stack, ToggleButton, ToggleButtonGroup, Dialog, DialogContent, DialogActions } from '@mui/material';
import { useNavigate, Link as RouterLink } from 'react-router-dom';
import { Card } from '@rbgrid/design-system';
import { useTelemetryEntities } from '../hooks/useTelemetryEntities';
import { useCapabilities, useConfigShadow } from '../core';
import { isOssBuild } from '../core/schema/capabilities';
import { useTariffPlan } from '../hooks/useTariffPlan';
import { useDeviceClock, formatDeviceTime } from '../core/deviceTime';
import { useSiteProfile, SITE_PROFILE_LABEL } from '../hooks/useSiteProfile';
import { useUnitsConfig } from '../hooks/useUnitsConfig';
import { useDeviceGroups } from '../hooks/useDeviceGroups';
import { useGridSystem } from '../hooks/useGridSystem';
import { useLocalStorage, useSessionData } from '../hooks/useLocalStorage';
import { PowerHero } from '../components/dashboard/PowerHero';
import { HubTimeNote } from '../components/dashboard/HubTimeNote';
import { SetupBanner } from '../components/dashboard/SetupBanner';
import { HubAccessWarning } from '../components/common/HubAccessWarning';
import { isDemo } from '../services/connection';
import { TodayCard } from '../components/dashboard/TodayCard';
import { PointTile } from '../components/dashboard/PointTile';
import { OverviewEnergyChart } from '../components/dashboard/OverviewEnergyChart';
import { LineLoadTable } from '../components/dashboard/LineLoadTable';
import { PhaseCards } from '../components/dashboard/PhaseCards';
import { AlarmsPanel } from '../components/dashboard/AlarmsPanel';
import { InputLimitHero } from '../components/dashboard/InputLimitHero';
import { PointsTable } from '../components/dashboard/PointsTable';
import { KpiRow, KpiItem } from '../components/dashboard/KpiRow';
import { TariffRibbon } from '../components/dashboard/TariffRibbon';
import { HomeFlowCard } from '../components/flow/HomeFlowCard';
import { EnergyFlowTree } from '../components/flow/EnergyFlowTree';
import { EnergyFlowSankey } from '../components/flow/EnergyFlowSankey';
import { EnergyFlowBalance } from '../components/flow/EnergyFlowBalance';
import { buildFlowFrame, solarShare } from '../components/flow/flowFrame';
import { buildFlowGroups } from '../components/flow/flowGroups';
import { fullPower, VBUS_KEY, apiUnitId, isMainKey, signedPowerW, isStorageRole } from '../entities/siteModel';
import { useMoneyFormat } from '../hooks/useMoneyFormat';
import { useDisplayMode } from '../hooks/useDisplayMode';
import { DisplayModeToggle } from '../components/common/DisplayModeToggle';
import { MoneyLensProvider, lensFormat } from '../economics/MoneyLens';
import { liveRate, moneyPerHour, dayAvgRate, solarDayOf } from '../economics/costRate';
import { SavingsCard } from '../components/dashboard/SavingsCard';
import { EvOverview } from '../components/stations/EvOverview';
import { evThresholds } from '../components/stations/evStatus';
import {
  buildIndustrialTree, mainUnit, nameOf, fullNameOf, lineUnits, phaseLoads, unmeteredW, offlinePoints,
  gridPhaseVoltages, pointUnits, isGenerationUnit,
} from './overviewData';
import { unitMetricValue, UnitEntityGroup } from '../entities/model';
import { EvConfig } from '../types/config';
import { enWh, pwW, nf, zoneLabel, qCharacter } from '../components/common/ruFormat';
import { t, intlLocale, dateLocale } from '../i18n';
import { useDsTokens } from '../theme/useDsTokens';

const byPowerDesc = (a: UnitEntityGroup, b: UnitEntityGroup): number => {
  if (a.present !== b.present) return a.present ? -1 : 1;
  return Math.abs(unitMetricValue(b, 'power') ?? 0) - Math.abs(unitMetricValue(a, 'power') ?? 0);
};


type ViewMode = 'overview' | 'analytics';
type FlowMode = 'sankey' | 'balance';

export const DashboardPage: React.FC = () => {
  const { data, conn, lastUpdate, error, authRejected } = useTelemetryEntities();
  const caps = useCapabilities();
  const ds = useDsTokens();
  const unitsCfg = useUnitsConfig();
  // Mains system (1P / split / 3P): the phase rows and nominals follow it.
  const grid = useGridSystem(unitsCfg);
  const { names: groupNames } = useDeviceGroups();
  const sysShadow = useConfigShadow('system');
  // The scenario is a DEVICE setting (shared by everyone) → read-only here; changed in
  // Настройки/Мастер with confirmation (review 04/01: the header toggle wrote to the device).
  const [scenario, , scenarioState] = useSiteProfile();
  const mf = useMoneyFormat(data?.system.totals?.tariff?.currency);
  // «Мощность ⇄ Деньги» lens: a per-user view preference, only when the device has a live tariff.
  // On the hub's day-cost basis: + the plan's levies and VAT (3φ audit L3P-02).
  const { plan: tariffPlan } = useTariffPlan();
  // The hub's clock for every time on the page (L3P-12: the stale banner was in the browser's zone).
  const hubClock = useDeviceClock();
  const rate = liveRate(data?.system.totals?.tariff, tariffPlan);
  const moneyAvailable = rate.buy != null && !(caps && isOssBuild(caps));
  const display = useDisplayMode(scenario, moneyAvailable);
  const lens = lensFormat({ mode: display.mode, rate, fmt: mf.money });
  // Kept for the tab's session: a remount (language, reconnect) or Back no longer resets «Details»
  // to «Summary» (3φ audit I3P-26).
  const [view, setView] = useSessionData<ViewMode>('rbgrid.dashboard_view', 'overview');
  const [profileOpen, setProfileOpen] = useState(false);
  const [flowMode, setFlowMode] = useLocalStorage<FlowMode>('rbgrid.analytics_flow', 'sankey');
  const navigate = useNavigate();

  // Phases the input has reported per-phase (U3I3 ph[]) this session — numbers only, NOT values:
  // when a frame drops ph[] (stale > 10 s) the card keeps its rows as «нет данных», no layout jump.
  const measuredLegs = useRef<number[]>([]);
  // seq-gap detector (spec §6: a jump > 2 ⇒ "data delayed").
  const prevSeq = useRef<number | undefined>(undefined);
  let seqGap = false;
  const seq = data?.system.seq;
  if (seq !== undefined) {
    if (prevSeq.current !== undefined && seq - prevSeq.current > 2) seqGap = true;
    prevSeq.current = seq;
  }

  const stale = conn === 'error';
  const frame = useMemo(
    () => (data
      ? buildFlowFrame(data, {
        lost: stale,
        roleByUnit: unitsCfg.roleByUnit,
        conductorsByUnit: unitsCfg.conductorsByUnit,
        nameOf: (g) => nameOf(g, unitsCfg),
        groups: buildFlowGroups(data.units.map((u) => {
          const c = unitsCfg.forGroup(u);
          return { key: u.unitKey, groupId: c?.groupId, trunk: c?.trunk, name: nameOf(u, unitsCfg) };
        }), groupNames),
      })
      : null),
    [data, stale, unitsCfg, groupNames],
  );

  // Today's closed hours from the history — a mid-day hub restart resets the live day counters.
  const mainEarly = data ? mainUnit(data.units, unitsCfg) : undefined;
  // The input's history id (HEX4) — the hub's or the demo's (9001), never a hardcoded '0006'
  // (rerun 16.10 HA-R01/HOME-R01: the demo's hour chart read a 60 W line).
  const mainHistId = mainEarly ? (unitsCfg.forGroup(mainEarly)?.hex ?? apiUnitId(mainEarly.unitKey)) : null;
  // The input's history series: all its supply points summed where the hub can (per-phase input).
  const inputSeries = useInputSeries(mainHistId);
  // The site's generation point (the first) for the house view of «Today by hour» (operator 20.10).
  const genEarly = data ? lineUnits(data.units, unitsCfg).find((g) => isGenerationUnit(g, unitsCfg) && g.present) : undefined;
  const genHistId = genEarly ? (unitsCfg.forGroup(genEarly)?.hex ?? apiUnitId(genEarly.unitKey)) : null;
  const genLiveOut = genEarly ? unitMetricValue(genEarly, 'energy_out') : null;
  // Month to date — closed days + today by the hub's counters, as Economics «месяц» (HOME-21).
  const monthTot = usePeriodTotals(inputSeries, 'month', { today: data?.system.totals?.e_day_in != null ? { inWh: Math.round(data.system.totals.e_day_in ?? 0), outWh: Math.round(data.system.totals.e_day_out ?? 0), cost: Math.round((data.system.totals.cost_day ?? 0) * 100) / 100, revenue: Math.round((data.system.totals.rev_day ?? 0) * 100) / 100, n: 1 } : null });

  if (!data || !frame) {
    return (
      <Box sx={{ display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', py: 10, gap: 2 }}>
        <CircularProgress aria-label={t('common.loading')} />
        <Typography color="text.secondary">{error ? t('dashboard.connecting') : t('dashboard.loadingTelemetry')}</Typography>
      </Box>
    );
  }

  const units = [...data.units].sort(byPowerDesc);
  const main = mainUnit(data.units, unitsCfg);
  if (main?.phases?.length) measuredLegs.current = main.phases.map((p) => p.ph);
  const vbus = data.units.find((u) => u.unitKey === VBUS_KEY);
  // Only a PRESENT unit's reading is a measurement; an absent bus reports v=0 → «—», not «0 В».
  const phaseV = (main?.phases ?? []).map((p) => p.v).filter((v): v is number => v != null);
  const busPhaseV = gridPhaseVoltages(data.units);
  const voltage = (main?.present ? unitMetricValue(main, 'voltage') : null)
    ?? (vbus?.present ? unitMetricValue(vbus, 'voltage') : null)
    // U3I3 input: mean L-N of its phases when the top-level v is omitted (.110, 82654dca).
    ?? (phaseV.length ? phaseV.reduce((a, b) => a + b, 0) / phaseV.length : null);
  // Frequency only when something measures it (UX audit INP-03/HEU-12: «50,00 Гц» on a hub with
  // no voltage source was the configured nominal). Older hubs lack freq_src → a present input/bus.
  const freqMeasured = data.system.freqSrc ? data.system.freqSrc === 'measured' : !!(main?.present || vbus?.present);
  const freq = freqMeasured ? data.system.freq ?? null : null;
  // Σ ph.p and the input's P must agree in sign (.110 with invert before canon 2b3dc1fd did not).
  const phSum = (main?.phases ?? []).reduce((s, p) => s + (p.p ?? 0), 0);
  const mainP = main ? unitMetricValue(main, 'power') : null;
  const phaseSignMismatch = !!main?.present && mainP != null && Math.abs(mainP) > 50 && Math.abs(phSum) > 50
    && Math.sign(phSum) !== Math.sign(mainP);
  // W9: a single-CT 3P / two-pole input reads one conductor → full power ≈ ×N (marked «≈»).
  const inputFull = main ? fullPower(main, unitsCfg.forGroup(main)) : null;
  const inputPowerW = inputFull?.w ?? null;
  const inputEstimated = !!inputFull?.estimated && inputPowerW != null;
  // system{} day/lifetime sums aggregate only the legacy ADC supply blocks; on a current install
  // (rbAmp input, no native ADC) they stay 0 until firmware T12 (root f451b6a5). There, take the day
  // energy from the input unit's own counters and show cost as unknown («—»), never a fake 0.
  const legacyInput = !!main && isMainKey(main.unitKey);
  const rawTotals = data.system.totals;
  // Day energy: the input's own counter, the system sum where the input has none (energy and
  // money of one figure must come from the same place — GEN-04 «+9,36 ₽ · ↑ 0,0 кВт·ч»).
  const pick = (unitWh: number | null, sysWh: number | null | undefined): number | null =>
    (unitWh != null && unitWh > 0) || sysWh == null ? unitWh : sysWh;
  const liveIn = main && !legacyInput ? pick(unitMetricValue(main, 'energy_day_in'), rawTotals?.e_day_in) : rawTotals?.e_day_in ?? null;
  const liveOut = main && !legacyInput ? pick(unitMetricValue(main, 'energy_day_out'), rawTotals?.e_day_out) : rawTotals?.e_day_out ?? null;
  // «Сегодня» = the hub's day counters, as on every screen (root, joint-hw-testing: they survive
  // restarts; the midnight-in-downtime fault that made the hours larger is fixed in ed876014).
  // No «larger of counter and hours» any more — it showed the Overview 1,8 kWh against Economics'
  // 0,0 after a clock change on .139.
  const dayIn = liveIn;
  const dayOut = liveOut;
  // One rule for «сегодня» money (UX audit T4): the hub's sum counts when it is non-zero or there
  // was no energy; a 0 next to real energy (hubs before canon 2b3dc1fd never costed rbAmp input)
  // is UNKNOWN → null, and the lens shows «≈ энергия × ставка» — never a fake «0,00 €».
  const trusted = (m: number | null | undefined, wh: number | null): number | null =>
    m != null && (m > 0 || !wh) ? m : null;
  const totals = rawTotals
    ? {
      ...rawTotals,
      e_day_in: dayIn,
      e_day_out: dayOut,
      cost_day: trusted(rawTotals.cost_day, dayIn),
      rev_day: trusted(rawTotals.rev_day, dayOut),
    } as typeof rawTotals
    : rawTotals;
  // Live tariff (E1). buy_rate 0 = no active plan → show no zone/rate rather than «0».
  const liveTariff = totals?.tariff;
  const tariffActive = (liveTariff?.buy_rate ?? 0) > 0;
  const tq = data.system.tq;
  const timeSynced = tq === undefined || tq === 1;
  const oss = caps ? isOssBuild(caps) : false;

  const evCfg = sysShadow.reported.ev as unknown as EvConfig | undefined;
  const ev = evThresholds(evCfg);
  const pricePerKwh = evCfg && evCfg.price_per_kwh > 0 ? evCfg.price_per_kwh : null;

  const isAnalytics = view === 'analytics';
  const titleText = isAnalytics ? t('dashboard.analytics') : t('dashboard.overview');

  const offlineNow = offlinePoints(data.units);
  const header = (
    <>
      {/* Phone-first: title, then a full-width view segment; side by side from md (820). */}
      <Stack direction={{ xs: 'column', md: 'row' }} alignItems={{ md: 'center' }} justifyContent="space-between" spacing={1.5} sx={{ mb: 0.5 }}>
        <Stack direction="row" alignItems="center" spacing={1.5}>
          {/* The page title = its menu item (HEU-15: «Обзор» page with an «Обзор» toggle). */}
          <Typography variant="h4" fontWeight={700}>{t('shell.navOverview')}</Typography>
          {seqGap && <Chip size="small" color="warning" label={t('dashboard.dataDelayed')} />}
        </Stack>
        <Stack direction="row" alignItems="center" spacing={1} flexWrap="wrap" useFlexGap sx={{ width: { xs: '100%', md: 'auto' } }}>
          {/* The site type is chosen right here (HOME-R11: the chip led into all of Settings, with
              «Заводской сброс» on the same page). */}
          <Chip
            size="small"
            variant="outlined"
            label={t('dashboard.scenarioChip', { scenario: SITE_PROFILE_LABEL[scenario] })}
            onClick={() => setProfileOpen(true)}
            clickable
          />
          <Dialog open={profileOpen} onClose={() => setProfileOpen(false)} maxWidth="md" fullWidth>
            <DialogContent><SiteProfilePanel /></DialogContent>
            <DialogActions><Button onClick={() => setProfileOpen(false)}>{t('common.close')}</Button></DialogActions>
          </Dialog>
          <DisplayModeToggle mode={display.mode} available={display.moneyAvailable} onChange={display.setMode} fullWidthOnPhone />
          <ToggleButtonGroup
            size="small" exclusive value={view}
            onChange={(_, v) => v && setView(v)} aria-label={t('dashboard.viewAria')}
            sx={{ flex: { xs: '1 1 100%', md: '0 0 auto' }, '& .MuiToggleButton-root': { flex: { xs: 1, md: 'initial' } } }}
          >
            <ToggleButton value="overview">{t('dashboard.overview')}</ToggleButton>
            <ToggleButton value="analytics">{t('dashboard.analytics')}</ToggleButton>
          </ToggleButtonGroup>
        </Stack>
      </Stack>
      <Typography variant="body2" color="text.secondary" sx={{ mb: 2 }}>
        {SITE_PROFILE_LABEL[scenario]} · {titleText}
      </Typography>
      {stale && (
        <Alert severity="warning" sx={{ mb: 2 }}>
          {authRejected ? t('dashboard.readTokenBanner') : t('dashboard.staleBanner', { time: lastUpdate ? formatDeviceTime(hubClock, Math.floor(lastUpdate / 1000), { hour: '2-digit', minute: '2-digit', second: '2-digit' }, dateLocale()) : '' })}
        </Alert>
      )}
      {!stale && !isDemo() && <SetupBanner placeholderTariff={liveTariff?.placeholder === true} />}
      {!stale && <HubAccessWarning />}
      {!stale && !isDemo() && <HubTimeNote />}
      {scenarioState.preview && (
        <Alert severity="info" sx={{ mb: 2 }}
          action={<Button color="inherit" size="small" onClick={() => scenarioState.setPreview(null)}>{t('siteProfile.previewOff')}</Button>}>
          {t('siteProfile.previewBanner', { profile: SITE_PROFILE_LABEL[scenarioState.preview], device: SITE_PROFILE_LABEL[scenarioState.deviceProfile] })}
        </Alert>
      )}
      {/* System health on the first screen (UX audit HOME-06: 4 of 5 sensors silent and not a word
          above the fold; «Тревог нет» in Events). */}
      {!stale && offlineNow.length > 0 && (
        <Alert
          severity="warning" sx={{ mb: 2 }}
          action={<Button color="inherit" size="small" sx={{ whiteSpace: 'nowrap' }} onClick={() => navigate('/events')}>{t('dashboard.offlineMore')}</Button>}
        >
          {t('dashboard.offlineStrip', { n: offlineNow.length, m: pointUnits(data.units).filter((g) => !g.noChannel).length })}
        </Alert>
      )}
    </>
  );

  // ---- empty state (no units at all) ----
  if (units.length === 0) {
    return (
      <Box>
        {header}
        <Card>
          <Stack spacing={1.5} alignItems="flex-start" sx={{ py: 2 }}>
            <Typography variant="h6" fontWeight={700}>{t('dashboard.emptyTitle')}</Typography>
            <Typography color="text.secondary">
              {t('dashboard.emptyText')}
            </Typography>
            <Button variant="contained" component={RouterLink} to="/onboarding">{t('dashboard.addSensor')}</Button>
          </Stack>
        </Card>
      </Box>
    );
  }

  const nameFor = (g: UnitEntityGroup) => fullNameOf(g, unitsCfg);
  const savings = (
    <SavingsCard lost={stale} industrial={scenario === 'industrial'} hubBase={data.system.totals ? { w: data.system.totals.base_w, nights: data.system.totals.base_w_nights } : null}
      groups={data.units}
      join={unitsCfg}
      mainId={main ? (unitsCfg.forGroup(main)?.hex ?? apiUnitId(main.unitKey)) : null}
      inputDayWh={totals?.e_day_in ?? (main ? unitMetricValue(main, 'energy_day_in') : null)}
      costDay={totals?.cost_day ?? null}
      exportDayWh={totals?.e_day_out ?? null}
      revDay={oss ? null : totals?.rev_day ?? null}
      rate={rate}
      fmt={mf.money}
    />
  );
  // Point tiles live ONLY in Дом, and only for lines: the main input, v_bus/alt and other
  // supply/pseudo units are already the hero (review 04 P0-1). Offline points sort last.
  const homeLines = lineUnits(units, unitsCfg);
  const tiles = (
    <Grid container spacing={2}>
      {homeLines.map((group) => (
        <Grid item xs={12} sm={6} lg={4} xl={3} key={group.unitKey}>
          <PointTile group={group} name={nameFor(group)} stale={stale} generation={isGenerationUnit(group, unitsCfg)} cfg={unitsCfg.forGroup(group)} onClick={() => navigate(`/point/${group.unitKey}`)} />
        </Grid>
      ))}
    </Grid>
  );

  // ---- HOME (§4.1) ----
  const renderHome = () => (
    <>
      {/* Without fresh frames the figures are the last ones — dimmed, never read as live (L3P-13). */}
      <Grid container spacing={2} sx={{ mb: 2, opacity: stale ? 0.55 : 1, transition: 'opacity .3s' }}>
        <Grid item xs={12} xl={6}><PowerHero frame={frame} voltage={voltage} phaseVoltages={busPhaseV} freq={freq} anyPresent={data.units.some((u) => u.present)} phaseSignMismatch={phaseSignMismatch} present={main?.present ?? false} staleS={main?.present && main.stale ? main.ageS ?? 6 : null}
          // rbamp-dev (root 86f39446): the hub treats < 0,5 W (and < 2 mA) as no flow — the same here
          // until p_res_w arrives; 5 W hid the Ф6 vampire steps of 4,7 and 0,54 W.
          idleW={main?.pResW != null ? Math.max(0.05, 2 * main.pResW) : 0.5} /></Grid>
        <Grid item xs={12} xl={6}>
          <TodayCard
            month={monthTot && !monthTot.loading && monthTot.n > 0 ? { wh: monthTot.inWh, cost: oss ? null : monthTot.cost } : null}
            energyInWh={totals?.e_day_in ?? null}
            energyOutWh={totals?.e_day_out ?? null}
            timeSynced={timeSynced}
            costDay={oss ? null : totals?.cost_day ?? null}
            revDay={oss ? null : totals?.rev_day ?? null}
            zoneName={tariffActive ? zoneLabel(liveTariff?.zone) : null}
            rateText={tariffActive ? `${mf.rate(rate.buy)}${rate.gross ? ` ${t('cards.rateGross')}` : ''}` : null}
            oss={oss}
            placeholderTariff={liveTariff?.placeholder === true}
          />
        </Grid>
      </Grid>
      {moneyAvailable && <Box sx={{ mb: 2 }}>{savings}</Box>}
      {/* A home on a declared multi-phase service (US split 120/240, 3P) or with a per-phase
          metered input (U3I3 ph[]) shows its legs. */}
      {((grid.fromDevice && grid.kind !== '1P') || measuredLegs.current.length > 1) && (
        <Box sx={{ mb: 2 }}><PhaseCards groups={data.units} join={unitsCfg} lost={stale} grid={grid} nominalV={grid.nominalV} measuredLegs={measuredLegs.current} /></Box>
      )}
      <Box sx={{ mb: 2 }}><HomeFlowCard frame={frame} phases={main?.phases && main.phases.length > 1 ? {
        gridW: main.present ? inputPowerW : null,
        legs: main.phases.map((r) => ({ ph: r.ph, p: r.p ?? null, i: r.i ?? null })),
        // Sources stay on their phase too — a 3φ home may have PV or a battery per phase (operator 16.10).
        lines: lineUnits(data.units, unitsCfg).map((g) => ({
          id: g.unitKey, name: nameFor(g), phase: unitsCfg.forGroup(g)?.phase,
          kind: isGenerationUnit(g, unitsCfg) ? 'gen' as const : isStorageRole(unitsCfg.forGroup(g)?.role) ? 'storage' as const : 'load' as const,
          w: g.present ? signedPowerW(g) : null, onOpen: () => navigate(`/point/${g.unitKey}`),
        })),
        lost: stale,
      } : null} /></Box>
      <Box sx={{ mb: 2 }}><OverviewEnergyChart hasGeneration={frame.solar != null} liveEIn={(main?.present ? unitMetricValue(main, 'energy_in') : null) ?? totals?.e_in ?? null} liveEOut={(main?.present ? unitMetricValue(main, 'energy_out') : null) ?? totals?.e_out ?? null} {...(mainHistId ? { unitId: mainHistId, seriesId: inputSeries } : {})} genUnitId={genHistId} liveGenOut={genLiveOut} /></Box>
      {tiles}
    </>
  );

  // ---- INDUSTRIAL (§4.2) ----
  const renderIndustrial = () => {
    const tree = buildIndustrialTree(data.units, unitsCfg, frame.grid, frame.gridHasDirection, stale, frame.groups, grid);
    // A deviation (offline point / alarm) lifts «Требует внимания» above the table on phones.
    const deviating = offlinePoints(data.units).length > 0
      || data.units.some((g) => g.attributes?.['alarm'] === true);
    return (
      <>
        <Box sx={{ mb: 2 }}>
          <InputLimitHero
            powerW={inputPowerW}
            estimated={inputEstimated}
            present={main?.present ?? false}
            // The input can't give more than its breaker passes (INP-R06: «из 43 кВт, запас 40,7» next
            // to «the breaker passes ≈ 14,5 kW» on the point page) — the smaller of the two.
            limitW={effectiveInputLimitW(unitsCfg.input.ratedPowerW, unitsCfg.input.maxCurrentA, unitsCfg.input.phase || (grid.kind === '3P' ? 4 : undefined), grid.nominalV)}
            breakerA={unitsCfg.input.maxCurrentA}
            breakerW={unitsCfg.input.maxCurrentA ? effectiveInputLimitW(undefined, unitsCfg.input.maxCurrentA, unitsCfg.input.phase || (grid.kind === '3P' ? 4 : undefined), grid.nominalV) : undefined}
            configLoaded={unitsCfg.loaded}
            freq={freq}
            phases={phaseLoads(data.units, unitsCfg, grid)}
            unmeteredW={unmeteredW(data.units, unitsCfg, inputPowerW)}
            lost={stale}
            // The input limit / breaker live on the input's point page (Settings links there too).
            settingsHref={main ? `/point/${unitsCfg.forGroup(main)?.hex ?? apiUnitId(main.unitKey) ?? main.unitKey}?setup` : '/devices'}
          />
        </Box>
        <Box sx={{ mb: 2 }}><PhaseCards groups={data.units} join={unitsCfg} lost={stale} grid={grid} nominalV={grid.nominalV} measuredLegs={measuredLegs.current} /></Box>
        <Grid container spacing={2} sx={{ mb: 2 }}>
          {/* The line table needs ~880 px for all its columns (INP-17: «Статус» was cut off at 1440) —
              side by side with the alarms only from xxl (1920). */}
          <Grid item xs={12} xxl={8} order={{ xs: deviating ? 2 : 1, xxl: 1 }}>
            <Card>
              <Typography variant="h6" fontWeight={700} sx={{ mb: 1 }}>{t('dashboard.loadLines')}</Typography>
              <LineLoadTable groups={units} join={unitsCfg} inputPowerW={inputPowerW} showRemainder={false} />
            </Card>
          </Grid>
          <Grid item xs={12} xxl={4} order={{ xs: deviating ? 1 : 2, xxl: 2 }}>
            <AlarmsPanel groups={data.units} title={t('dashboard.attention')} nameOf={nameFor} ratedOf={(g) => (isMainKey(g.unitKey) || unitsCfg.forGroup(g)?.role === 'MAIN_SUPPLY' || isGenerationUnit(g, unitsCfg) ? undefined : unitsCfg.forGroup(g)?.ratedPowerW)} />
          </Grid>
        </Grid>
        <Box sx={{ mb: 2 }}><Card><EnergyFlowTree data={tree} /></Card></Box>
      </>
    );
  };

  // ---- EV STATIONS (§4.3) ----
  const renderEv = () => (
    <EvOverview
      units={data.units}
      join={unitsCfg}
      ev={ev}
      grid={frame.grid}
      gridHasDirection={frame.gridHasDirection}
      pricePerKwh={pricePerKwh}
      formatMoney={(v) => mf.money(v)}
      lost={stale}
      onOpenPoint={(key) => navigate(`/point/${key}`)}
    />
  );

  // ---- ANALYTICS (§4.4), shared across scenarios ----
  const renderAnalytics = () => {
    const sharePct = Math.round(solarShare(frame) * 100);
    const hasGen = frame.solar != null;
    // KPI only where data can exist (review 04 §3.4): without generation there is no
    // «Отдано»/«Автономность»; «Пик сегодня» has no source yet → not drawn; cos φ only when
    // the input reports it.
    const pf = main?.present ? unitMetricValue(main, 'power_factor') : null;
    // Every money/energy KPI follows the «Мощность ⇄ Деньги» lens (useLensFormat); in money mode
    // «Взято» already is the day's cost, so the 4th tile becomes the day's net (cost − revenue).
    const inNow = lens.power(main?.present ? inputPowerW : null);
    const took = lens.energy(totals?.e_day_in ?? null, false, oss ? null : totals?.cost_day ?? null);
    const gave = lens.energy(totals?.e_day_out ?? null, true, oss ? null : totals?.rev_day ?? null);
    const kpis: KpiItem[] = [
      // Says the direction (it read «Из сети сейчас ↑ 300 Вт» while exporting on .139).
      { label: (inputPowerW ?? 0) < 0 && main?.present ? t('cards.heroExporting') : t('dashboard.kpiInputNow'), value: `${inputEstimated && main?.present ? '≈ ' : ''}${inNow.main}`, sub: main && !main.present ? t('dashboard.noLink') : inNow.sub ?? undefined },
      { label: t('dashboard.kpiImportedToday'), value: took.main, sub: timeSynced ? took.sub ?? undefined : t('dashboard.timeNotSynced') },
    ];
    if (hasGen) kpis.push({ label: t('dashboard.kpiExportedToday'), value: gave.main, sub: gave.sub ?? undefined, valueColor: gave.money ? 'var(--flow-export)' : undefined, subColor: 'var(--flow-export)' });
    const cost = oss ? null : totals?.cost_day ?? null;
    const rev = oss ? null : totals?.rev_day ?? null;
    if (lens.moneyMode && cost != null && hasGen && rev != null) {
      kpis.push({ label: t('dashboard.kpiNetToday'), value: mf.money(cost - rev), sub: t('dashboard.kpiNetSub') });
    } else if (!lens.moneyMode || !took.money) {
      kpis.push({ label: t('dashboard.kpiCostToday'), value: oss ? '—' : mf.money(cost) });
    }
    if (hasGen) kpis.push({ label: t('dashboard.kpiAutonomy'), value: `${sharePct}%`, sub: t('dashboard.fromSolar'), valueColor: ds.solarInk });
    // With its character (I3P-03): «0.95 leading» — the sign of the input's Q.
    else if (pf != null) { const ch = qCharacter(main?.present ? unitMetricValue(main, 'reactive_power') : null, inputPowerW); kpis.push({ label: t('dashboard.kpiInputPf'), value: `${nf(pf, 2)}${ch ? ` ${ch}` : ''}` }); }
    return (
      <>
        <Box sx={{ mb: 2 }}><KpiRow items={kpis} /></Box>
        {/* A multi-phase input keeps its phases in «Подробно» too (3φ audit H3P-22: switching to
            «more detail» lost the per-phase load). */}
        {((grid.fromDevice && grid.kind !== '1P') || measuredLegs.current.length > 1) && (
          <Box sx={{ mb: 2 }}><PhaseCards groups={data.units} join={unitsCfg} lost={stale} grid={grid} nominalV={grid.nominalV} measuredLegs={measuredLegs.current} /></Box>
        )}
        <Grid container spacing={2} sx={{ mb: 2 }}>
          <Grid item xs={12} lg={8} sx={{ alignSelf: 'flex-start' }}><OverviewEnergyChart hasGeneration={frame.solar != null} liveEIn={(main?.present ? unitMetricValue(main, 'energy_in') : null) ?? totals?.e_in ?? null} liveEOut={(main?.present ? unitMetricValue(main, 'energy_out') : null) ?? totals?.e_out ?? null} {...(mainHistId ? { unitId: mainHistId, seriesId: inputSeries } : {})} genUnitId={genHistId} liveGenOut={genLiveOut} /></Grid>
          <Grid item xs={12} lg={4}>
            <Stack spacing={2}>
              <AlarmsPanel groups={data.units} title={t('dashboard.attention')} nameOf={nameFor} ratedOf={(g) => (isMainKey(g.unitKey) || unitsCfg.forGroup(g)?.role === 'MAIN_SUPPLY' || isGenerationUnit(g, unitsCfg) ? undefined : unitsCfg.forGroup(g)?.ratedPowerW)} />
              {moneyAvailable && savings}
              <TariffRibbon currentZone={tariffActive ? liveTariff?.zone ?? null : null} gross={rate.gross} />
            </Stack>
          </Grid>
        </Grid>
        <Box sx={{ mb: 2 }}>
          <Card>
            <Stack direction="row" alignItems="center" justifyContent="space-between" flexWrap="wrap" useFlexGap gap={1} sx={{ mb: 1 }}>
              <Typography variant="h6" fontWeight={700}>{t('dashboard.energyFlow')}</Typography>
              <ToggleButtonGroup size="small" exclusive value={flowMode} onChange={(_, v) => v && setFlowMode(v)} aria-label={t('dashboard.flowSchemeAria')}>
                <ToggleButton value="sankey">{t('dashboard.flowSankey')}</ToggleButton>
                <ToggleButton value="balance">{t('dashboard.flowBalance')}</ToggleButton>
              </ToggleButtonGroup>
            </Stack>
            {flowMode === 'sankey' ? <EnergyFlowSankey frame={frame} /> : <EnergyFlowBalance frame={frame} />}
          </Card>
        </Box>
        <Box sx={{ mb: 2 }}>
          <Card>
            <Typography variant="h6" fontWeight={700} sx={{ mb: 1 }}>{t('dashboard.points')}</Typography>
            <PointsTable groups={data.units} join={unitsCfg} lost={stale}
              inputToday={{ wh: totals?.e_day_in ?? null, money: oss ? null : totals?.cost_day ?? null }} />
          </Card>
        </Box>
      </>
    );
  };

  return (
    <MoneyLensProvider mode={display.mode} rate={rate} fmt={mf.money} gridW={main?.present && frame.gridHasDirection ? frame.grid * 1000 : null}
      dayRate={dayAvgRate(oss ? null : totals)}
      solarDay={solarDayOf(data.units.filter((g) => isGenerationUnit(g, unitsCfg)).reduce((s, g) => s + (unitMetricValue(g, 'energy_day_out') ?? 0), 0),
        oss ? null : totals, dayAvgRate(oss ? null : totals).buy ?? rate.buy, rate.sell)}>
      <Box>
        {header}
        {isAnalytics ? renderAnalytics() : scenario === 'home' ? renderHome() : scenario === 'industrial' ? renderIndustrial() : renderEv()}
      </Box>
    </MoneyLensProvider>
  );
};
