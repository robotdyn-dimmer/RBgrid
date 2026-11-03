/**
 * ChartLegend — colour key for charts and flow schemas (operator 2026-10-13: «в карточках не
 * хватает легенды по цветовой идентификации»). Identity is never colour-alone: every mark has
 * a text label; text stays in muted ink (never the series colour). Marks mirror what the chart
 * draws: a filled box (bars/segments), a hatched box (partial / current period), a line, a
 * dashed line (limits / no data), a ring dot (partial point).
 */
import React from 'react';
import { Box, Stack, Typography } from '@mui/material';
import { useDsTokens } from '../../theme/useDsTokens';
import { t } from '../../i18n';

export type LegendMark = 'box' | 'hatch' | 'line' | 'dash' | 'ring' | 'faded' | 'outline' | 'ghost';
export interface LegendItem { label: string; color: string; mark?: LegendMark }

const Swatch: React.FC<{ color: string; mark: LegendMark; surface: string }> = ({ color, mark, surface }) => {
  const box = { width: 12, height: 12, borderRadius: '3px', flexShrink: 0 };
  switch (mark) {
    case 'hatch':
      return <Box sx={{ ...box, border: `1px solid ${color}`, background: `repeating-linear-gradient(45deg, ${color} 0 2px, transparent 2px 5px)` }} />;
    case 'line':
      return <Box sx={{ width: 16, height: 2, borderRadius: 1, bgcolor: color, flexShrink: 0 }} />;
    case 'dash':
      return <Box sx={{ width: 16, height: 0, borderTop: `2px dashed ${color}`, flexShrink: 0 }} />;
    case 'outline':
      // a partial interval: a hollow bar (charts audit 17.10 — paleness meant two things)
      return <Box sx={{ ...box, border: `1.5px solid ${color}`, bgcolor: 'transparent' }} />;
    case 'ghost':
      // the previous period: the same colour, thinner and quieter
      return <Box sx={{ width: 16, height: 1.5, borderRadius: 1, bgcolor: color, opacity: 0.45, flexShrink: 0 }} />;
    case 'faded':
      return <Box sx={{ ...box, bgcolor: color, opacity: 0.45 }} />;
    case 'ring':
      return <Box sx={{ width: 10, height: 10, borderRadius: '50%', border: `2px solid ${color}`, bgcolor: surface, flexShrink: 0 }} />;
    default:
      return <Box sx={{ ...box, bgcolor: color }} />;
  }
};

export const ChartLegend: React.FC<{ items: LegendItem[]; sx?: object }> = ({ items, sx }) => {
  const ds = useDsTokens();
  if (items.length === 0) return null;
  return (
    <Stack component="ul" direction="row" flexWrap="wrap" useFlexGap columnGap={2} rowGap={0.75}
      aria-label={t('common.legend')} sx={{ listStyle: 'none', m: 0, p: 0, ...sx }}>
      {items.map((it) => (
        <Stack component="li" key={it.label} direction="row" alignItems="center" spacing={0.75}>
          <Swatch color={it.color} mark={it.mark ?? 'box'} surface={ds.surface} />
          <Typography sx={{ fontSize: 12, color: ds.inkMuted, whiteSpace: 'nowrap' }}>{it.label}</Typography>
        </Stack>
      ))}
    </Stack>
  );
};
