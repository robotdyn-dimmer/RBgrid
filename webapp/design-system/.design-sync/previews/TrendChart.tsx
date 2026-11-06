import React from 'react';
import { TrendChart } from '@rbgrid/design-system';
import { Box } from '@mui/material';

const HOURS = Array.from({ length: 24 }, (_, i) => `${String(i).padStart(2, '0')}:00`);
// A plausible daily load curve (W): low overnight, morning + evening peaks.
const LOAD = [
  420, 380, 360, 350, 360, 540, 980, 1650, 2100, 1850, 1600, 1700,
  1900, 1750, 1500, 1650, 2200, 3100, 3450, 3200, 2600, 1800, 1100, 620,
];

export const Electrical = () => (
  <Box sx={{ width: 760 }}>
    <TrendChart
      labels={HOURS}
      series={[{ label: 'Power (W)', color: '#60a5fa', data: LOAD }]}
      band={{ min: LOAD.map((v) => v * 0.82), max: LOAD.map((v) => v * 1.18) }}
    />
  </Box>
);

export const Energy = () => (
  <Box sx={{ width: 760 }}>
    <TrendChart
      labels={['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun']}
      series={[
        { label: 'Consumed (Wh)', color: '#f59e0b', data: [14200, 13800, 15100, 12600, 16700, 9400, 8800] },
        { label: 'Supplied (Wh)', color: '#34d399', data: [3100, 2800, 4200, 3600, 5100, 6700, 7200] },
      ]}
    />
  </Box>
);
