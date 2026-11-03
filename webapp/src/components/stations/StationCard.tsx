/**
 * StationCard — one EV charging station on the «Станции» overview (spec §4.3).
 *
 * Shows the station name, its DERIVED status (always with the «определено по мощности»
 * tooltip — U3), current power and the running session's kWh. No connector state, no
 * «ограничено»/«Ошибка» (those need phase-2 integration).
 *
 * Money lens («Мощность ⇄ Деньги»): with a station sell price (system.ev.price_per_kwh) the hero
 * becomes ≈ revenue per hour (power × price), power moves under it; with a live buy rate the
 * margin per hour is shown too. All «≈» — estimates at current power and rates.
 */
import React from 'react';
import { Box, Stack, Typography, Tooltip } from '@mui/material';
import { Card } from '@rbgrid/design-system';
import { useDsTokens } from '../../theme/useDsTokens';
import { dsFonts } from '../../theme/dsTokens';
import { StationStatus, STATION_STATUS, stationStatusTooltip } from './evStatus';
import { t } from '../../i18n';
import { nf } from '../common/ruFormat';
import { useMoneyLens } from '../../economics/MoneyLens';

/** Power W → "7,4 кВт" / "320 Вт" / "—". */
function fmtPower(w: number | null | undefined): string {
  if (w == null) return '—';
  const a = Math.abs(w);
  return a >= 1000 ? `${nf(w / 1000, 1)} ${t('units.kW')}` : `${nf(w)} ${t('units.W')}`;
}

export interface StationView {
  id: string;
  name: string;
  powerW: number | null;
  /** W9: single-CT multi-phase station — powerW is the ×N balance estimate. */
  estimated?: boolean;
  present: boolean;
  status: StationStatus;
  sessionKwh?: number | null;
  maxPowerW?: number | null;
}

export interface StationCardProps {
  station: StationView;
  onClick?: () => void;
  /** Station sell price per kWh (device currency); null = not set → no revenue figures. */
  pricePerKwh?: number | null;
}

export const StationCard: React.FC<StationCardProps> = ({ station, onClick, pricePerKwh = null }) => {
  const ds = useDsTokens();
  const lens = useMoneyLens();
  // W9: «≈» on a single-CT multi-phase station's ×N estimate.
  const est = station.estimated && station.powerW != null && station.present ? '≈ ' : '';
  const kw = station.powerW != null && station.present ? Math.max(0, station.powerW) / 1000 : null;
  const money = lens.mode === 'money' && pricePerKwh != null;
  const revenueH = money && kw != null ? kw * pricePerKwh! : null;
  const marginH = money && kw != null && lens.rate.buy != null ? kw * (pricePerKwh! - lens.rate.buy) : null;
  const perH = (v: number) => `${t('common.approx')} ${lens.fmt(v)}${t('units.perHour')}`;
  const statusColor: Record<StationStatus, string> = {
    charging: ds.statusOnline,
    waiting: ds.sevWarning,
    free: ds.inkMuted,
    nodata: ds.statusOffline,
  };
  const color = statusColor[station.status];
  const mono = { fontFamily: dsFonts.mono, fontVariantNumeric: 'tabular-nums' as const };

  return (
    <Box
      onClick={onClick}
      sx={{ cursor: onClick ? 'pointer' : 'default', height: '100%' }}
    >
      <Card>
        <Stack spacing={1.25} sx={{ py: 0.5 }}>
          <Stack direction="row" alignItems="center" justifyContent="space-between" spacing={1}>
            <Typography sx={{ fontSize: 15, fontWeight: 600, color: station.present ? ds.ink : ds.inkMuted }} noWrap>{station.name}</Typography>
            <Tooltip title={stationStatusTooltip()} arrow>
              <Box
                component="span"
                sx={{
                  flexShrink: 0, px: 1, py: '2px', borderRadius: 999, fontSize: 11, lineHeight: 1.6,
                  color, border: `1px solid ${color}`, whiteSpace: 'nowrap',
                }}
              >
                {STATION_STATUS[station.status].label}
              </Box>
            </Tooltip>
          </Stack>

          {money ? (
            <Box>
              <Typography sx={{ ...mono, fontSize: 30, fontWeight: 600, letterSpacing: '-0.01em', color: station.present ? ds.ink : ds.inkMuted, lineHeight: 1.1 }}>
                {revenueH != null ? perH(revenueH) : t('common.dash')}
              </Typography>
              <Typography sx={{ ...mono, fontSize: 12, color: ds.inkMuted, mt: 0.5 }} title={est ? t('cards.balanceEstimate') : undefined}>
                {est}{fmtPower(station.powerW)}{marginH != null ? ` · ${t('stations.marginPerHour', { value: perH(marginH) })}` : ''}
              </Typography>
            </Box>
          ) : (
            <Typography sx={{ ...mono, fontSize: 30, fontWeight: 600, letterSpacing: '-0.01em', color: station.present ? ds.ink : ds.inkMuted, lineHeight: 1.1 }} title={est ? t('cards.balanceEstimate') : undefined}>
              {est}{fmtPower(station.powerW)}
            </Typography>
          )}

          <Stack direction="row" spacing={3}>
            <Stack spacing={0.25}>
              <Typography sx={{ fontSize: 11, letterSpacing: '0.02em' }} color="text.secondary">{t('stations.today')}</Typography>
              <Typography sx={{ ...mono, fontSize: 14, fontWeight: 600, color: ds.ink }}>
                {station.sessionKwh != null ? `${nf(station.sessionKwh, 1)} ${t('units.kWh')}` : t('common.dash')}
              </Typography>
            </Stack>
            {station.maxPowerW != null && (
              <Stack spacing={0.25}>
                <Typography sx={{ fontSize: 11, letterSpacing: '0.02em' }} color="text.secondary">{t('stations.limitLabel')}</Typography>
                <Typography sx={{ ...mono, fontSize: 14, fontWeight: 600, color: ds.inkMuted }}>
                  {fmtPower(station.maxPowerW)}
                </Typography>
              </Stack>
            )}
          </Stack>
        </Stack>
      </Card>
    </Box>
  );
};
