import React from 'react';
import { ChannelCard } from '@rbgrid/design-system';
import { Stack, Box } from '@mui/material';

export const Channels = () => (
  <Box sx={{ width: 420 }}>
    <Stack spacing={1.5}>
      <ChannelCard
        protocol="mqtt"
        enabled
        sendIntervalMs={5000}
        config={{ broker: 'mqtt://192.168.0.10:1883', topic: 'rbgrid/telemetry', qos: '1' }}
      />
      <ChannelCard protocol="websocket" enabled sendIntervalMs={1000} />
      <ChannelCard protocol="http" enabled={false} />
    </Stack>
  </Box>
);
