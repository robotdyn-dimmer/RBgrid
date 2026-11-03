import React from 'react';
import { Box, Typography } from '@mui/material';
import { tokens } from '../tokens';

export interface GaugeProps {
  /** Current value. */
  value: number;
  /** Range minimum (default 0). */
  min?: number;
  /** Range maximum (default 100). */
  max?: number;
  /** Unit suffix shown under the value (e.g. "W", "Hz"). */
  unit?: string;
  /** Caption under the gauge. */
  label?: string;
  /** Arc color (defaults to the brand primary). */
  color?: string;
  /** Diameter in px (default 160). */
  size?: number;
}

/**
 * A 270° radial gauge for a single live metric (power, frequency, load). Pure SVG —
 * no chart dependency. The arc fills proportionally to value within [min, max].
 *
 * @category Dashboard
 */
export const Gauge: React.FC<GaugeProps> = ({
  value,
  min = 0,
  max = 100,
  unit,
  label,
  color = tokens.color.primary,
  size = 160,
}) => {
  const START = 135; // degrees — bottom-left
  const SWEEP = 270;
  const clamped = Math.max(min, Math.min(max, value));
  const frac = max > min ? (clamped - min) / (max - min) : 0;
  const r = size / 2 - 12;
  const cx = size / 2;
  const cy = size / 2;

  const polar = (deg: number) => {
    const rad = (deg * Math.PI) / 180;
    return { x: cx + r * Math.cos(rad), y: cy + r * Math.sin(rad) };
  };
  const arc = (fromDeg: number, toDeg: number) => {
    const a = polar(fromDeg);
    const b = polar(toDeg);
    const large = Math.abs(toDeg - fromDeg) > 180 ? 1 : 0;
    return `M ${a.x} ${a.y} A ${r} ${r} 0 ${large} 1 ${b.x} ${b.y}`;
  };

  return (
    <Box sx={{ display: 'inline-flex', flexDirection: 'column', alignItems: 'center' }}>
      <Box sx={{ position: 'relative', width: size, height: size }}>
        <svg width={size} height={size} viewBox={`0 0 ${size} ${size}`}>
          <path
            d={arc(START, START + SWEEP)}
            fill="none"
            stroke={tokens.color.border}
            strokeWidth={12}
            strokeLinecap="round"
          />
          <path
            d={arc(START, START + SWEEP * frac)}
            fill="none"
            stroke={color}
            strokeWidth={12}
            strokeLinecap="round"
          />
        </svg>
        <Box
          sx={{
            position: 'absolute',
            inset: 0,
            display: 'flex',
            flexDirection: 'column',
            alignItems: 'center',
            justifyContent: 'center',
          }}
        >
          <Typography variant="h4" fontWeight={800} lineHeight={1}>
            {Number.isFinite(value) ? value.toLocaleString() : '—'}
          </Typography>
          {unit && (
            <Typography variant="caption" color="text.secondary">
              {unit}
            </Typography>
          )}
        </Box>
      </Box>
      {label && (
        <Typography variant="body2" color="text.secondary" sx={{ mt: 0.5 }}>
          {label}
        </Typography>
      )}
    </Box>
  );
};
