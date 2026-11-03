import React from 'react';
import { Card as MuiCard, CardContent } from '@mui/material';

export interface CardProps {
  /** Card contents. */
  children?: React.ReactNode;
  /** Dim the card to signal an inactive / offline / dormant entity. */
  muted?: boolean;
  /** Outlined (no elevation) variant — for secondary / nested cards. */
  outlined?: boolean;
}

/**
 * Base surface for RBGrid — the HA-style "ha-card": 12px radius, soft border,
 * comfortable padding. Every panel/card composes from this.
 *
 * @category Surfaces
 */
export const Card: React.FC<CardProps> = ({ children, muted = false, outlined = false }) => (
  <MuiCard
    variant={outlined ? 'outlined' : 'elevation'}
    sx={{ height: '100%', borderRadius: 3, opacity: muted ? 0.5 : 1 }}
  >
    <CardContent>{children}</CardContent>
  </MuiCard>
);
