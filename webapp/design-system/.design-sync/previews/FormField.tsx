import React from 'react';
import { FormField } from '@rbgrid/design-system';
import { Stack, Box } from '@mui/material';

export const Form = () => (
  <Box sx={{ width: 360 }}>
    <Stack spacing={2}>
      <FormField label="Home name" value="RBGrid Home" onChange={() => {}} />
      <FormField label="SSID" value="RBGrid-Home" onChange={() => {}} />
      <FormField label="Password" type="password" value="supersecret" onChange={() => {}} />
      <FormField
        label="Role"
        value="DEVICE_LOAD"
        onChange={() => {}}
        options={[
          { value: 'DEVICE_LOAD', label: 'Device load' },
          { value: 'MAIN_FEED', label: 'Main feed' },
          { value: 'SOLAR', label: 'Solar' },
        ]}
      />
    </Stack>
  </Box>
);
