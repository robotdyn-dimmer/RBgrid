import React from 'react';
import { StatTile } from '@rbgrid/design-system';
import { Stack } from '@mui/material';

export const Headline = () => (
  <StatTile label="Power" value="2.83 kW" sub="Importing" subColor="warning.main" emphasis />
);

export const Compact = () => <StatTile label="Frequency" value="50.0 Hz" />;

export const Strip = () => (
  <Stack direction="row" spacing={5}>
    <StatTile label="Power" value="2.83 kW" sub="Importing" subColor="warning.main" emphasis />
    <StatTile label="Frequency" value="50.0 Hz" />
    <StatTile label="Units" value="6/8" />
    <StatTile label="Today in" value="14.2 kWh" />
    <StatTile label="Today out" value="3.1 kWh" subColor="success.main" sub="exported" />
  </Stack>
);
