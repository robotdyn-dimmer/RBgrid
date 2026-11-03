import React from 'react';
import { Box, Stack, Typography } from '@mui/material';
import { StatusChip } from './StatusChip';
import { SettingRow } from './SettingRow';

export interface ChannelCardProps {
  /** Transport protocol (e.g. "mqtt", "http"). */
  protocol: string;
  /** Whether the channel is enabled. */
  enabled: boolean;
  /** Send interval in milliseconds (optional). */
  sendIntervalMs?: number | null;
  /** Channel config key→value pairs (already stringified). */
  config?: Record<string, string>;
}

/**
 * A telemetry channel summary — protocol, on/off, interval, and config rows.
 * @category Settings
 */
export const ChannelCard: React.FC<ChannelCardProps> = ({
  protocol,
  enabled,
  sendIntervalMs,
  config,
}) => (
  <Box sx={{ border: 1, borderColor: 'divider', borderRadius: 2, p: 1.5 }}>
    <Stack direction="row" alignItems="center" spacing={1.5} mb={config ? 1 : 0}>
      <Typography variant="subtitle2" fontWeight={700}>
        {protocol}
      </Typography>
      <StatusChip status={enabled ? 'present' : 'dormant'} label={enabled ? 'on' : 'off'} />
      {sendIntervalMs != null && (
        <Typography variant="caption" color="text.secondary">
          every {sendIntervalMs} ms
        </Typography>
      )}
    </Stack>
    {config &&
      Object.entries(config).map(([k, v]) => <SettingRow key={k} label={k} value={v} />)}
  </Box>
);
