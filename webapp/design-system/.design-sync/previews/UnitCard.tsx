import React from 'react';
import { UnitCard } from '@rbgrid/design-system';
import { Box } from '@mui/material';

export const Importing = () => (
  <Box sx={{ width: 280 }}>
    <UnitCard name="Main feed" powerW={2831} voltageV={229.4} currentA={12.34} powerFactor={0.98} source="i2c" unitId="9020" />
  </Box>
);

export const HighPower = () => (
  <Box sx={{ width: 280 }}>
    <UnitCard name="EV charger" powerW={7240} voltageV={231.1} currentA={31.4} powerFactor={1.0} source="i2c" unitId="9031" />
  </Box>
);

export const Offline = () => (
  <Box sx={{ width: 280 }}>
    <UnitCard name="Garage subpanel" powerW={null} present={false} source="adc" unitId="03" />
  </Box>
);
