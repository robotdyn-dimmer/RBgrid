import React from 'react';
import { ToggleRow } from '@rbgrid/design-system';
import { Stack } from '@mui/material';

export const Settings = () => (
  <Stack>
    <ToggleRow label="Telemetry enabled" checked onChange={() => {}} />
    <ToggleRow label="Auto-start measurements" checked={false} onChange={() => {}} />
    <ToggleRow label="Logging enabled" checked onChange={() => {}} />
    <ToggleRow label="Compact JSON" checked={false} disabled />
  </Stack>
);
