import React from 'react';
import { SystemSummary } from '@rbgrid/design-system';
import { Box } from '@mui/material';

export const Importing = () => (
  <Box sx={{ width: 760 }}>
    <SystemSummary
      connection="live"
      stats={[
        { label: 'Power', value: '2.83 kW', sub: 'Importing', subColor: 'warning.main', emphasis: true },
        { label: 'Frequency', value: '50.0 Hz' },
        { label: 'Units', value: '6/8' },
        { label: 'Today in', value: '14.2 kWh' },
        { label: 'Today out', value: '3.1 kWh', subColor: 'success.main', sub: 'exported' },
        { label: 'Cost today', value: '1.86' },
      ]}
    />
  </Box>
);

export const Exporting = () => (
  <Box sx={{ width: 760 }}>
    <SystemSummary
      connection="live"
      stats={[
        { label: 'Power', value: '1.12 kW', sub: 'Exporting', subColor: 'success.main', emphasis: true },
        { label: 'Frequency', value: '49.98 Hz' },
        { label: 'Units', value: '8/8' },
        { label: 'Today in', value: '9.4 kWh' },
        { label: 'Today out', value: '6.7 kWh', subColor: 'success.main', sub: 'exported' },
      ]}
    />
  </Box>
);
