/**
 * LoadBalanceBar — parking power vs the input limit (spec §4.3.2).
 *
 * One bar: «Инфраструктура» + per-station segments against `limitW`; the free remainder is
 * hatched. A word summary on the right states headroom / load WITHOUT claiming any station
 * controller is balancing (that needs phase-2 integration) — so at high load it only says
 * what share of the limit the stations are drawing.
 */
import React from 'react';
import { Box, Stack, Typography, Tooltip } from '@mui/material';
import { useDsTokens } from '../../theme/useDsTokens';
import { dsFonts } from '../../theme/dsTokens';
import { t } from '../../i18n';
import { nf } from '../common/ruFormat';

const kw = (w: number) => (Math.abs(w) >= 1000 ? `${nf(w / 1000, 1)} ${t('units.kW')}` : `${nf(w)} ${t('units.W')}`);

export interface LoadSegment {
  id: string;
  name: string;
  valueW: number;
  kind: 'infra' | 'station';
}

export interface LoadBalanceBarProps {
  segments: LoadSegment[];
  limitW: number;
}

export const LoadBalanceBar: React.FC<LoadBalanceBarProps> = ({ segments, limitW }) => {
  const ds = useDsTokens();
  const limit = limitW > 0 ? limitW : 1;
  const used = segments.reduce((s, g) => s + Math.max(0, g.valueW), 0);
  const freeW = Math.max(0, limit - used);
  const usedPct = Math.min(100, (used / limit) * 100);
  const headroomPct = Math.max(0, 100 - (used / limit) * 100);

  // Stable colour per segment (infra = muted, stations = series order) for bar + legend.
  const colors = new Map<string, string>();
  let stationIdx = 0;
  for (const g of segments) {
    if (g.kind === 'infra') colors.set(g.id, ds.inkMuted);
    else { colors.set(g.id, ds.series[stationIdx % ds.series.length]); stationIdx += 1; }
  }
  const colorFor = (g: LoadSegment): string => colors.get(g.id) ?? ds.inkMuted;

  // Word summary (no integration claim): near the ceiling → «у предела» + share drawn; else headroom.
  const nearLimit = used / limit >= 0.92;
  const summary = nearLimit
    ? t('stations.nearLimit', { pct: nf(usedPct) })
    : t('stations.headroom', { power: kw(freeW) });

  const hatch = `repeating-linear-gradient(45deg, ${ds.border} 0, ${ds.border} 2px, transparent 2px, transparent 6px)`;

  return (
    <Box>
      <Stack direction={{ xs: 'column', sm: 'row' }} alignItems={{ sm: 'center' }} spacing={1.5}>
        <Box
          sx={{ position: 'relative', flex: { xs: '0 0 auto', sm: 1 }, width: { xs: '100%', sm: 'auto' }, display: 'flex', height: 22, borderRadius: 6, overflow: 'hidden', bgcolor: ds.surfaceRaised, minWidth: 180 }}
          role="img"
          aria-label={t('stations.barAria', { used: kw(used), limit: kw(limit) })}
        >
          {segments.map((g) => {
            const w = (Math.max(0, g.valueW) / limit) * 100;
            if (w <= 0) return null;
            return (
              <Tooltip key={g.id} title={`${g.name}: ${kw(g.valueW)}`} arrow>
                <Box role="img" aria-label={`${g.name}: ${kw(g.valueW)}`} sx={{ width: `${w}%`, bgcolor: colorFor(g), borderRight: `2px solid ${ds.surface}` }} />
              </Tooltip>
            );
          })}
          {freeW > 0 && (
            <Tooltip title={t('stations.freeTip', { power: kw(freeW) })} arrow>
              <Box role="img" aria-label={t('stations.freeTip', { power: kw(freeW) })} sx={{ flex: 1, backgroundImage: hatch }} />
            </Tooltip>
          )}
        </Box>
        <Typography sx={{ fontSize: 13, fontWeight: 600, color: nearLimit || headroomPct < 8 ? ds.sevWarning : ds.ink, whiteSpace: 'nowrap' }}>
          {summary}
        </Typography>
      </Stack>
      <Stack direction="row" flexWrap="wrap" useFlexGap columnGap={2} rowGap={0.5} sx={{ mt: 0.75 }}>
        <Typography sx={{ fontSize: 12, fontFamily: dsFonts.mono, fontVariantNumeric: 'tabular-nums', color: ds.ink, fontWeight: 600 }}>
          {t('stations.usedOfLimit', { used: kw(used), limit: kw(limit) })}
        </Typography>
        {segments.filter((g) => g.valueW > 0).map((g) => (
          <Stack key={g.id} direction="row" spacing={0.75} alignItems="center">
            <Box sx={{ width: 8, height: 8, borderRadius: '50%', bgcolor: colorFor(g), flexShrink: 0 }} />
            <Typography sx={{ fontSize: 12, color: ds.inkMuted, fontVariantNumeric: 'tabular-nums', whiteSpace: 'nowrap' }}>
              {g.name} {kw(g.valueW)}
            </Typography>
          </Stack>
        ))}
      </Stack>
    </Box>
  );
};
