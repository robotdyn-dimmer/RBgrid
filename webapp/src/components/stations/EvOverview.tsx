/**
 * EvOverview — the «Станции» Overview layout (spec §4.3, review 04 §5.4, persona 09).
 *
 * Order (desktop and phone alike — the primary answer first):
 *   1. ParkingHero — «N из M заряжают» + load balance vs the input limit (one parking-power
 *      number, one definition);
 *   2. KPI — Энергия сегодня · Выручка сегодня (only when a price is set) · Сессий сегодня;
 *   3. StationCard grid (3 / 2 / 1 per row);
 *   4. «Мощность паркинга · сегодня» — line + limit (sev-critical);
 *   5. EnergyFlowTree (stations vs infrastructure).
 * No point tiles, no home hourly chart; solar/export only appear if a generation line exists.
 * A component (not a render function) so it can own the history hook.
 */
import React from 'react';
import { Alert, Box, Grid } from '@mui/material';
import { Card } from '@rbgrid/design-system';
import { UnitEntityGroup } from '../../entities/model';
import { UnitsConfigJoin } from '../../hooks/useUnitsConfig';
import { KpiRow, KpiItem } from '../dashboard/KpiRow';
import { EnergyFlowTree } from '../flow/EnergyFlowTree';
import { StationCard } from './StationCard';
import { ParkingHero } from './ParkingHero';
import { ParkingPowerChart, useParkingHistory } from './ParkingPowerChart';
import { EvThresholds } from './evStatus';
import {
  buildEvTree, buildStations, buildLoadSegments, stationCounts, stationSets, lineUnits, isStation, isGenerationUnit,
} from '../../pages/overviewData';
import { useDeviceGroups } from '../../hooks/useDeviceGroups';
import { enWh } from '../common/ruFormat';
import { useLensFormat } from '../../economics/MoneyLens';
import { t } from '../../i18n';

export interface EvOverviewProps {
  units: UnitEntityGroup[];
  join: UnitsConfigJoin;
  ev: EvThresholds;
  /** Grid power for the tree root (kW) + whether it carries a direction. */
  grid: number;
  gridHasDirection: boolean;
  /** Price per kWh for EV sales; null = not set → the revenue KPI isn't drawn. */
  pricePerKwh: number | null;
  formatMoney: (v: number | null) => string;
  lost: boolean;
  onOpenPoint: (key: string) => void;
}

export const EvOverview: React.FC<EvOverviewProps> = ({
  units, join, ev, grid, gridHasDirection, pricePerKwh, formatMoney, lost, onOpenPoint,
}) => {
  const { names: groupNames } = useDeviceGroups();
  const stations = buildStations(units, join, ev, groupNames);
  const counts = stationCounts(stations);
  const offline = stations.filter((s) => s.status === 'nodata').length;
  const segments = buildLoadSegments(units, join, groupNames);
  // Input limit from the device only (main unit rated_power_w) — never substituted (U3).
  const limitW = join.input.ratedPowerW;

  // Parking lines = the balance bar's set (stations + non-generation infrastructure).
  const parking = lineUnits(units, join).filter((g) => isStation(g, join) || !isGenerationUnit(g, join));
  const history = useParkingHistory(
    parking.map((g) => g.unitKey),
    stationSets(units, join).map((st) => st.keys),
    ev.chargeThresholdW,
    join.input.hex,
  );

  const evEnergyTodayKwh = stations.reduce((s, st) => s + (st.sessionKwh ?? 0), 0);
  const tree = buildEvTree(units, join, grid, gridHasDirection, lost);

  const lens = useLensFormat();
  const evWh = evEnergyTodayKwh * 1000;
  const revenue = pricePerKwh != null ? evEnergyTodayKwh * pricePerKwh : null;
  const purchase = lens.energyMoney(evWh); // ≈ energy × live buy rate
  const kpis: KpiItem[] = [];
  if (lens.moneyMode && revenue != null) {
    // Money lens: what the parking earned and what is left after the energy bought for it.
    const buyCost = lens.rate.buy != null ? (evWh / 1000) * lens.rate.buy : null;
    kpis.push({ label: t('stations.revenueToday'), value: formatMoney(revenue), sub: enWh(evWh) });
    if (buyCost != null) {
      kpis.push({ label: t('stations.marginToday'), value: `${t('common.approx')} ${formatMoney(revenue - buyCost)}`, sub: t('stations.purchase', { money: purchase }) });
    }
  } else {
    const e = lens.energy(evWh);
    kpis.push({ label: t('stations.energyToday'), value: e.main, sub: e.money ? e.sub ?? undefined : t('stations.allStations') });
    // §14 W4: no price → the revenue KPI is not drawn (not «0 ₽», not «—»).
    if (revenue != null) {
      kpis.push({ label: t('stations.revenueToday'), value: formatMoney(revenue), sub: t('stations.atSalePrice') });
    }
  }
  kpis.push(history.sessions != null
    ? { label: t('stations.sessionsToday'), value: `${history.sessions}`, sub: t('stations.sessionsEstimate') }
    : { label: t('stations.chargingNow'), value: `${counts.charging}`, sub: t('stations.sessionsUnavailable') });

  return (
    <>
      <Box sx={{ mb: 2 }}>
        <ParkingHero
          charging={counts.charging} total={counts.total} free={counts.free} offline={offline}
          segments={segments} limitW={limitW} lost={lost}
        />
      </Box>
      <Box sx={{ mb: 2 }}><KpiRow items={kpis} /></Box>
      {stations.length > 0 ? (
        <Grid container spacing={2} sx={{ mb: 2 }}>
          {stations.map((st) => (
            <Grid item xs={12} sm={6} lg={4} key={st.id}>
              <StationCard station={st} pricePerKwh={pricePerKwh} onClick={() => onOpenPoint(st.id)} />
            </Grid>
          ))}
        </Grid>
      ) : (
        <Alert severity="info" sx={{ mb: 2 }}>{t('stations.noStations')}</Alert>
      )}
      <Box sx={{ mb: 2 }}><ParkingPowerChart history={history} limitW={limitW} /></Box>
      <Box sx={{ mb: 2 }}><Card><EnergyFlowTree data={tree} /></Card></Box>
    </>
  );
};
