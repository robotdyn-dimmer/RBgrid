/**
 * ParkingHero — the Станции primary block (spec §4.3.1–2, review 04 §5.4 #1): «N из M
 * заряжают» XL with the load-balance bar folded in. ONE number for parking power and ONE
 * definition (stations + infrastructure = the bar's Σ) — the KPI row no longer repeats it.
 *
 * Statuses are DERIVED from power (U3) → the «определено по мощности» caption is always shown.
 * No input limit → «лимит ввода не задан» + a settings link; never a substituted limit.
 */
import React from 'react';
import { Link as RouterLink } from 'react-router-dom';
import { Box, Stack, Typography, Link, Tooltip } from '@mui/material';
import { Card } from '@rbgrid/design-system';
import { useDsTokens } from '../../theme/useDsTokens';
import { LoadBalanceBar, LoadSegment } from './LoadBalanceBar';
import { stationStatusTooltip } from './evStatus';
import { pwW } from '../common/ruFormat';
import { t } from '../../i18n';

export interface ParkingHeroProps {
  charging: number;
  total: number;
  free: number;
  offline: number;
  segments: LoadSegment[];
  /** Input limit (W); undefined = not set. */
  limitW?: number;
  /** Frame lost → greyed. */
  lost?: boolean;
  settingsHref?: string;
}

export const ParkingHero: React.FC<ParkingHeroProps> = ({
  charging, total, free, offline, segments, limitW, lost, settingsHref = '/devices',
}) => {
  const ds = useDsTokens();
  const parkingW = segments.reduce((s, g) => s + Math.max(0, g.valueW), 0);
  const parts = [t('stations.freeCount', { count: String(free) })];
  if (offline > 0) parts.push(t('stations.offlineCount', { count: String(offline) }));

  return (
    <Card>
      <Box
        sx={{
          display: 'grid', gap: { xs: 2, md: 3 }, py: 0.5, alignItems: 'center', opacity: lost ? 0.6 : 1,
          gridTemplateColumns: { xs: '1fr', md: 'minmax(180px, 3fr) minmax(0, 7fr)' },
        }}
      >
        <Stack spacing={0.5}>
          <Typography sx={{ fontSize: 12, letterSpacing: '0.02em', color: ds.inkMuted }}>{t('stations.chargingNow')}</Typography>
          <Typography sx={{ fontVariantNumeric: 'tabular-nums', fontSize: { xs: 36, md: 44 }, fontWeight: 600, letterSpacing: '-0.02em', lineHeight: 1.05 }}>
            {charging}
            <Box component="span" sx={{ fontSize: '0.5em', fontWeight: 500, color: ds.inkMuted }}> {t('stations.ofTotal', { total })}</Box>
          </Typography>
          <Typography sx={{ fontSize: 13, color: offline > 0 ? ds.statusOffline : ds.inkMuted }}>{parts.join(' · ')}</Typography>
          <Tooltip title={t('stations.measuredTooltip')} arrow>
            <Typography sx={{ fontSize: 11, color: ds.inkMuted, width: 'fit-content' }}>{stationStatusTooltip()}</Typography>
          </Tooltip>
        </Stack>

        <Stack spacing={1} sx={{ minWidth: 0 }}>
          <Typography sx={{ fontWeight: 700, fontSize: 15 }}>{t('stations.balanceTitle')}</Typography>
          {limitW != null ? (
            <LoadBalanceBar segments={segments} limitW={limitW} />
          ) : (
            <>
              <Typography sx={{ fontVariantNumeric: 'tabular-nums', fontSize: 22, fontWeight: 600 }}>{pwW(parkingW)}</Typography>
              <Typography sx={{ fontSize: 13, color: ds.inkMuted }}>
                {t('stations.limitNotSetDash')}{' '}
                <Link component={RouterLink} to={settingsHref} underline="hover">{t('stations.setRatedPower')}</Link>
                {t('stations.toSeeHeadroom')}
              </Typography>
            </>
          )}
        </Stack>
      </Box>
    </Card>
  );
};
