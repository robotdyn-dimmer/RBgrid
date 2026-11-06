import React from 'react';
import { EntityCard } from '@rbgrid/design-system';
import { Box } from '@mui/material';

export const Live = () => (
  <Box sx={{ width: 280 }}>
    <EntityCard
      name="Main feed"
      value="2.83 kW"
      present
      metrics={[
        { label: 'Voltage', value: '229.4 V' },
        { label: 'Current', value: '12.34 A' },
        { label: 'Power factor', value: '0.98' },
      ]}
      caption="I2C · 9020"
    />
  </Box>
);

export const Offline = () => (
  <Box sx={{ width: 280 }}>
    <EntityCard
      name="Garage subpanel"
      value="—"
      present={false}
      metrics={[
        { label: 'Voltage', value: '—' },
        { label: 'Current', value: '—' },
        { label: 'Power factor', value: '—' },
      ]}
      caption="ADC · ch 3"
    />
  </Box>
);
