import React from 'react';
import { Box, Typography } from '@mui/material';

export interface InfoFieldProps {
  /** Field caption (e.g. "SSID", "IP"). */
  label: string;
  /** Field value, already formatted. */
  value: string;
}

/**
 * A compact label-over-value field — the atom of status/glance rows.
 * @category Primitives
 */
export const InfoField: React.FC<InfoFieldProps> = ({ label, value }) => (
  <Box>
    <Typography variant="caption" color="text.secondary" display="block">
      {label}
    </Typography>
    <Typography variant="body2" fontWeight={600}>
      {value}
    </Typography>
  </Box>
);
