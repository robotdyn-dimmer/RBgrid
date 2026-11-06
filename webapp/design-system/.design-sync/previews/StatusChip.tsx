import React from 'react';
import { StatusChip } from '@rbgrid/design-system';
import { Stack } from '@mui/material';

export const Live = () => <StatusChip status="live" />;
export const Offline = () => <StatusChip status="offline" />;
export const Error = () => <StatusChip status="error" />;

export const AllStates = () => (
  <Stack direction="row" spacing={1} flexWrap="wrap" useFlexGap>
    <StatusChip status="live" />
    <StatusChip status="present" label="on" />
    <StatusChip status="dormant" label="off" />
    <StatusChip status="offline" />
    <StatusChip status="error" />
  </Stack>
);
