import React from 'react';
import { Box, Stack } from '@mui/material';
import { Card } from './Card';
import { StatTile } from './StatTile';
import { StatusChip, StatusKind } from './StatusChip';

export interface SummaryStat {
  /** Caption above the value. */
  label: string;
  /** Formatted value (e.g. "2.83 kW"). */
  value: string;
  /** Optional sub-line (direction / detail). */
  sub?: string;
  /** Sub-line color. */
  subColor?: 'text.secondary' | 'success.main' | 'warning.main' | 'error.main';
  /** Render this tile as the large headline KPI. */
  emphasis?: boolean;
}

export interface SystemSummaryProps {
  /** KPI tiles, left-to-right (power, frequency, units, energy, cost…). */
  stats: SummaryStat[];
  /** Connection status pill shown on the right. */
  connection?: StatusKind;
  /** Optional connection label override. */
  connectionLabel?: string;
}

/**
 * Dashboard system header — a strip of headline KPIs (power, frequency, units,
 * today's energy/cost) with a connection status pill. The top of the dashboard.
 *
 * @category Dashboard
 */
export const SystemSummary: React.FC<SystemSummaryProps> = ({
  stats,
  connection,
  connectionLabel,
}) => (
  <Card>
    <Stack direction="row" spacing={5} alignItems="center" flexWrap="wrap" useFlexGap>
      {stats.map((s) => (
        <StatTile
          key={s.label}
          label={s.label}
          value={s.value}
          sub={s.sub}
          subColor={s.subColor}
          emphasis={s.emphasis}
        />
      ))}
      {connection && (
        <Box sx={{ ml: 'auto' }}>
          <StatusChip status={connection} label={connectionLabel ?? connection} />
        </Box>
      )}
    </Stack>
  </Card>
);
