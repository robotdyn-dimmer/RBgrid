/**
 * StackedBar — one horizontal bar of parts with 2 px gaps, labels «Night €0.50 · 30 %» under it and
 * the total at the right (charts spec 17.10: «where the money goes» at one glance — rows of numbers
 * made the reader add them up). Colour follows the part's identity, never its rank.
 */
import React from 'react';
import { Box, Stack, Typography } from '@mui/material';
import { useDsTokens } from '../../theme/useDsTokens';

export interface StackPart { key: string; label: string; value: number; color: string; muted?: boolean }

export const StackedBar: React.FC<{ parts: StackPart[]; fmt: (v: number) => string; total?: string; ariaLabel?: string }> = ({ parts, fmt, total, ariaLabel }) => {
  const ds = useDsTokens();
  const shown = parts.filter((p) => p.value > 0);
  const sum = shown.reduce((a, p) => a + p.value, 0);
  if (sum <= 0) return null;
  const pct = (v: number) => Math.round((v / sum) * 100);
  return (
    <Box role="img" aria-label={ariaLabel ?? shown.map((p) => `${p.label} ${fmt(p.value)} (${pct(p.value)} %)`).join(', ')}>
      <Stack direction="row" alignItems="center" spacing={1}>
        <Box sx={{ display: 'flex', flex: 1, height: 14, gap: '2px', borderRadius: '4px', overflow: 'hidden' }}>
          {shown.map((p) => (
            <Box key={p.key} title={`${p.label}: ${fmt(p.value)} · ${pct(p.value)} %`}
              sx={{ flex: `${p.value} 0 0`, minWidth: 3, bgcolor: p.color, opacity: p.muted ? 0.4 : 0.9 }} />
          ))}
        </Box>
        {total && <Typography sx={{ fontWeight: 700, fontVariantNumeric: 'tabular-nums', whiteSpace: 'nowrap' }}>{total}</Typography>}
      </Stack>
      <Stack direction="row" flexWrap="wrap" useFlexGap columnGap={2} rowGap={0.5} sx={{ mt: 0.75 }}>
        {shown.map((p) => (
          <Stack key={p.key} direction="row" alignItems="center" spacing={0.75}>
            <Box sx={{ width: 10, height: 10, borderRadius: '2px', bgcolor: p.color, opacity: p.muted ? 0.4 : 0.9, flexShrink: 0 }} />
            <Typography sx={{ fontSize: 13, color: ds.ink, fontVariantNumeric: 'tabular-nums' }}>
              {p.label} <Box component="span" sx={{ color: ds.inkMuted }}>{fmt(p.value)} · {pct(p.value)} %</Box>
            </Typography>
          </Stack>
        ))}
      </Stack>
    </Box>
  );
};

/** A tariff zone's colour by its identity (the same zone, the same colour on every screen). */
const ZONE_ORDER = ['NIGHT', 'DAY', 'PEAK', 'OFF_PEAK', 'SHOULDER', 'SUPER_OFF_PEAK', 'CRITICAL_PEAK', 'WEEKEND', 'HOLIDAY', 'CUSTOM'];
export const zoneColorOf = (zone: string, series: readonly string[]): string => {
  const i = ZONE_ORDER.indexOf(String(zone).toUpperCase());
  return series[(i < 0 ? ZONE_ORDER.length : i) % series.length];
};
