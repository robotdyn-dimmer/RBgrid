import React from 'react';
import { Gauge } from '@rbgrid/design-system';
import { Stack } from '@mui/material';

export const Power = () => <Gauge value={2831} min={0} max={8000} unit="W" label="Active power" />;

export const Frequency = () => (
  <Gauge value={50} min={45} max={55} unit="Hz" label="Line frequency" color="#34d399" />
);

export const Trio = () => (
  <Stack direction="row" spacing={3}>
    <Gauge value={2831} min={0} max={8000} unit="W" label="Power" size={130} />
    <Gauge value={50} min={45} max={55} unit="Hz" label="Frequency" color="#34d399" size={130} />
    <Gauge value={86} min={0} max={100} unit="%" label="Load" color="#fbbf24" size={130} />
  </Stack>
);
