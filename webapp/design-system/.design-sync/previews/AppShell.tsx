import React from 'react';
import { AppShell, SystemSummary, StatusChip } from '@rbgrid/design-system';
import { Box } from '@mui/material';

export const Dashboard = () => (
  <Box sx={{ width: 900, height: 520, overflow: 'hidden', borderRadius: 2 }}>
    <AppShell
      brand="RBGrid"
      headerRight={<StatusChip status="live" />}
      nav={[
        { label: 'Dashboard', active: true },
        { label: 'Devices' },
        { label: 'History' },
        { label: 'Tariffs' },
        { label: 'Settings' },
      ]}
    >
      <SystemSummary
        connection="live"
        stats={[
          { label: 'Power', value: '2.83 kW', sub: 'Importing', subColor: 'warning.main', emphasis: true },
          { label: 'Frequency', value: '50.0 Hz' },
          { label: 'Units', value: '6/8' },
          { label: 'Today in', value: '14.2 kWh' },
        ]}
      />
    </AppShell>
  </Box>
);
