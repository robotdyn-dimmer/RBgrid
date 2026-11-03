import React from 'react';
import { Box, Typography } from '@mui/material';

export interface SettingRowProps {
  /** Key / setting name. */
  label: string;
  /** Value, already stringified. */
  value: string;
}

/**
 * A justified key→value row for read-only config listings.
 * @category Primitives
 */
export const SettingRow: React.FC<SettingRowProps> = ({ label, value }) => (
  <Box sx={{ display: 'flex', justifyContent: 'space-between', py: 0.5, gap: 2 }}>
    <Typography variant="body2" color="text.secondary">
      {label}
    </Typography>
    <Typography variant="body2" fontWeight={600} sx={{ textAlign: 'right', wordBreak: 'break-all' }}>
      {value}
    </Typography>
  </Box>
);
