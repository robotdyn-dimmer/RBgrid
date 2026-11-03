import React from 'react';
import { Chip } from '@mui/material';

export type StatusKind = 'live' | 'offline' | 'present' | 'dormant' | 'error';

export interface StatusChipProps {
  /** The status to display. */
  status: StatusKind;
  /** Optional label override (defaults to the status name). */
  label?: string;
}

const COLOR: Record<StatusKind, 'success' | 'default' | 'error'> = {
  live: 'success',
  present: 'success',
  offline: 'default',
  dormant: 'default',
  error: 'error',
};

/**
 * Small outlined status pill (live / offline / present / dormant / error).
 * @category Surfaces
 */
export const StatusChip: React.FC<StatusChipProps> = ({ status, label }) => (
  <Chip size="small" variant="outlined" color={COLOR[status]} label={label ?? status} />
);
