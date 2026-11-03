import React from 'react';
import { Box, Typography, Stack } from '@mui/material';
import { Card } from './Card';
import { StatusChip } from './StatusChip';

export interface EntityMetric {
  /** Metric label (e.g. "Voltage"). */
  label: string;
  /** Formatted value (e.g. "230 V"). */
  value: string;
}

export interface EntityCardProps {
  /** Entity / device name. */
  name: string;
  /** Headline value, formatted (e.g. "642 W"). */
  value: string;
  /** Whether the entity is present / live (false → muted + "offline"). */
  present?: boolean;
  /** Secondary metrics shown in a glance row. */
  metrics?: EntityMetric[];
  /** Footer caption (e.g. "I2C · 9020"). */
  caption?: string;
}

/**
 * HA glance-style entity card: name + status pill, a headline value, a row of
 * secondary metrics, and a caption. The building block of the dashboard grid.
 *
 * @category Dashboard
 */
export const EntityCard: React.FC<EntityCardProps> = ({
  name,
  value,
  present = true,
  metrics = [],
  caption,
}) => (
  <Card muted={!present}>
    <Box sx={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', mb: 1.5 }}>
      <Typography variant="subtitle1" fontWeight={700} noWrap>
        {name}
      </Typography>
      <StatusChip status={present ? 'live' : 'offline'} />
    </Box>
    <Typography variant="h4" fontWeight={800} sx={{ mb: 1.5 }}>
      {value}
    </Typography>
    {metrics.length > 0 && (
      <Stack direction="row" spacing={3} flexWrap="wrap" useFlexGap>
        {metrics.map((m) => (
          <Box key={m.label}>
            <Typography variant="caption" color="text.secondary" display="block">
              {m.label}
            </Typography>
            <Typography variant="body2" fontWeight={600}>
              {m.value}
            </Typography>
          </Box>
        ))}
      </Stack>
    )}
    {caption && (
      <Typography variant="caption" color="text.secondary" sx={{ display: 'block', mt: 1.5 }}>
        {caption}
      </Typography>
    )}
  </Card>
);
