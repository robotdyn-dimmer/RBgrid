import React from 'react';
import { Box, Typography } from '@mui/material';

export interface StatTileProps {
  /** Caption above the value. */
  label: string;
  /** The value, already formatted (e.g. "2.83 kW", "50.0 Hz"). */
  value: string;
  /** Optional sub-line (trend / direction / detail). */
  sub?: string;
  /** Color of the sub-line. */
  subColor?: 'text.secondary' | 'success.main' | 'warning.main' | 'error.main';
  /** Render large (headline KPI) vs compact. */
  emphasis?: boolean;
}

/**
 * A single labelled metric (KPI tile) — the atom of the system header / stat strips.
 * @category Primitives
 */
export const StatTile: React.FC<StatTileProps> = ({
  label,
  value,
  sub,
  subColor = 'text.secondary',
  emphasis = false,
}) => (
  <Box>
    <Typography variant="caption" color="text.secondary">
      {label}
    </Typography>
    <Typography variant={emphasis ? 'h3' : 'h5'} fontWeight={emphasis ? 800 : 600} lineHeight={1.1}>
      {value}
    </Typography>
    {sub && (
      <Typography variant="body2" color={subColor}>
        {sub}
      </Typography>
    )}
  </Box>
);
