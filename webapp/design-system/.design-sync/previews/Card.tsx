import React from 'react';
import { Card } from '@rbgrid/design-system';
import { Typography } from '@mui/material';

export const Default = () => (
  <Card>
    <Typography variant="subtitle1" fontWeight={700}>Surface</Typography>
    <Typography variant="body2" color="text.secondary">
      The ha-card base — 12px radius, soft border, comfortable padding.
    </Typography>
  </Card>
);

export const Outlined = () => (
  <Card outlined>
    <Typography variant="subtitle1" fontWeight={700}>Nested / secondary</Typography>
    <Typography variant="body2" color="text.secondary">Outlined, no elevation.</Typography>
  </Card>
);

export const Muted = () => (
  <Card muted>
    <Typography variant="subtitle1" fontWeight={700}>Offline entity</Typography>
    <Typography variant="body2" color="text.secondary">Dimmed to signal dormant state.</Typography>
  </Card>
);
