import React from 'react';
import { NetworkStatus } from '@rbgrid/design-system';
import { Box } from '@mui/material';

export const Connected = () => (
  <Box sx={{ width: 480 }}>
    <NetworkStatus mode="STA" ssid="RBGrid-Home" ip="192.168.0.110" rssi={-54} />
  </Box>
);

export const AccessPoint = () => (
  <Box sx={{ width: 480 }}>
    <NetworkStatus mode="AP" ssid="RBGrid-Setup" ip="192.168.4.1" rssi={null} />
  </Box>
);
