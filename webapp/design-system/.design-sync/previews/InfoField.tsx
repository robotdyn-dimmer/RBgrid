import React from 'react';
import { InfoField } from '@rbgrid/design-system';
import { Stack } from '@mui/material';

export const Default = () => <InfoField label="IP" value="192.168.0.110" />;

export const Row = () => (
  <Stack direction="row" spacing={3}>
    <InfoField label="Mode" value="STA" />
    <InfoField label="SSID" value="RBGrid-Home" />
    <InfoField label="IP" value="192.168.0.110" />
    <InfoField label="RSSI" value="-54 dBm" />
  </Stack>
);
