import React from 'react';
import { Card as MuiCard, CardContent, Typography, Button, Stack, Alert } from '@mui/material';

export interface PanelCardProps {
  /** Panel heading (e.g. "Status", "Time / NTP", "Telemetry"). */
  title: string;
  /** Optional action button label (e.g. "Scan", "Sync now"). */
  actionLabel?: string;
  /** Action button icon (optional ReactNode). */
  actionIcon?: React.ReactNode;
  /** Disable the action button. */
  actionDisabled?: boolean;
  /** Action click handler. */
  onAction?: () => void;
  /** Optional inline alert above the body. */
  alert?: string;
  /** Alert severity. */
  alertSeverity?: 'success' | 'info' | 'warning' | 'error';
  /** Panel body. */
  children?: React.ReactNode;
}

/**
 * The settings-panel shell — a titled ha-card with an optional header action
 * button and an optional inline alert. Every settings panel composes from this.
 *
 * @category Settings
 */
export const PanelCard: React.FC<PanelCardProps> = ({
  title,
  actionLabel,
  actionIcon,
  actionDisabled = false,
  onAction,
  alert,
  alertSeverity = 'info',
  children,
}) => (
  <MuiCard sx={{ borderRadius: 3 }}>
    <CardContent>
      <Stack direction="row" alignItems="center" mb={1.5}>
        <Typography variant="h6" fontWeight={700}>
          {title}
        </Typography>
        {actionLabel && (
          <Button startIcon={actionIcon} onClick={onAction} disabled={actionDisabled} sx={{ ml: 'auto' }}>
            {actionLabel}
          </Button>
        )}
      </Stack>
      {alert && (
        <Alert severity={alertSeverity} sx={{ mb: 2 }}>
          {alert}
        </Alert>
      )}
      {children}
    </CardContent>
  </MuiCard>
);
