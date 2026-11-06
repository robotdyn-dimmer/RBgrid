import React from 'react';
import { PanelCard, NetworkStatus, ToggleRow } from '@rbgrid/design-system';
import { Stack, Box } from '@mui/material';

export const WithAction = () => (
  <Box sx={{ width: 520 }}>
    <PanelCard title="Time / NTP" actionLabel="Sync now">
      <NetworkStatus mode="STA" ssid="RBGrid-Home" ip="192.168.0.110" rssi={-54} />
    </PanelCard>
  </Box>
);

export const WithAlert = () => (
  <Box sx={{ width: 520 }}>
    <PanelCard title="System" alert="System settings saved." alertSeverity="success">
      <Stack>
        <ToggleRow label="Telemetry enabled" checked onChange={() => {}} />
        <ToggleRow label="Auto-start measurements" checked={false} onChange={() => {}} />
      </Stack>
    </PanelCard>
  </Box>
);
