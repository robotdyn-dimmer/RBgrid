import React from 'react';
import { Box, Stack, Typography } from '@mui/material';
import { tokens } from '../tokens';

export interface TrendSeries {
  /** Series label (legend). */
  label: string;
  /** Line color. */
  color: string;
  /** Y values, left-to-right (null = gap). */
  data: (number | null)[];
}

export interface TrendBand {
  /** Lower envelope values. */
  min: (number | null)[];
  /** Upper envelope values. */
  max: (number | null)[];
  /** Band fill color (defaults to a translucent primary). */
  color?: string;
}

export interface TrendChartProps {
  /** X-axis labels (also defines the point count). */
  labels: string[];
  /** One or more series to plot. */
  series: TrendSeries[];
  /** Optional min/max envelope band (HA statistics-graph style). */
  band?: TrendBand;
  /** Chart height in px (default 320). */
  height?: number;
}

/**
 * A multi-series line chart with an optional min/max envelope band — the HA
 * statistics-graph look for history/analytics. Pure SVG, no chart dependency.
 *
 * @category Dashboard
 */
export const TrendChart: React.FC<TrendChartProps> = ({ labels, series, band, height = 320 }) => {
  const W = 800;
  const H = height;
  const padX = 8;
  const padY = 16;
  const n = labels.length;

  const all: number[] = [];
  for (const s of series) for (const v of s.data) if (v != null) all.push(v);
  if (band) for (const v of [...band.min, ...band.max]) if (v != null) all.push(v);
  let lo = all.length ? Math.min(...all) : 0;
  let hi = all.length ? Math.max(...all) : 1;
  if (lo === hi) { lo -= 1; hi += 1; }

  const x = (i: number) => (n <= 1 ? padX : padX + (i * (W - 2 * padX)) / (n - 1));
  const y = (v: number) => padY + (1 - (v - lo) / (hi - lo)) * (H - 2 * padY);

  const linePath = (data: (number | null)[]) => {
    let d = '';
    data.forEach((v, i) => {
      if (v == null) { d += ''; return; }
      d += `${d && data[i - 1] != null ? 'L' : 'M'} ${x(i).toFixed(1)} ${y(v).toFixed(1)} `;
    });
    return d.trim();
  };

  const bandPath = () => {
    if (!band) return '';
    const top = band.max.map((v, i) => (v == null ? null : `${x(i).toFixed(1)} ${y(v).toFixed(1)}`));
    const bot = band.min
      .map((v, i) => (v == null ? null : `${x(i).toFixed(1)} ${y(v).toFixed(1)}`))
      .reverse();
    const pts = [...top.filter(Boolean), ...bot.filter(Boolean)];
    return pts.length ? `M ${pts.join(' L ')} Z` : '';
  };

  return (
    <Box>
      <Box sx={{ width: '100%', height: H }}>
        <svg width="100%" height={H} viewBox={`0 0 ${W} ${H}`} preserveAspectRatio="none">
          {band && <path d={bandPath()} fill={band.color ?? 'rgba(96,165,250,0.15)'} stroke="none" />}
          {series.map((s) => (
            <path
              key={s.label}
              d={linePath(s.data)}
              fill="none"
              stroke={s.color}
              strokeWidth={2}
              strokeLinejoin="round"
              strokeLinecap="round"
            />
          ))}
        </svg>
      </Box>
      <Stack direction="row" spacing={2} flexWrap="wrap" useFlexGap sx={{ mt: 1 }}>
        {series.map((s) => (
          <Stack key={s.label} direction="row" alignItems="center" spacing={0.75}>
            <Box sx={{ width: 12, height: 3, borderRadius: 2, bgcolor: s.color }} />
            <Typography variant="caption" color="text.secondary">
              {s.label}
            </Typography>
          </Stack>
        ))}
      </Stack>
      {labels.length > 0 && (
        <Stack direction="row" justifyContent="space-between" sx={{ mt: 0.5 }}>
          <Typography variant="caption" color={tokens.color.textSecondary}>
            {labels[0]}
          </Typography>
          <Typography variant="caption" color={tokens.color.textSecondary}>
            {labels[labels.length - 1]}
          </Typography>
        </Stack>
      )}
    </Box>
  );
};
