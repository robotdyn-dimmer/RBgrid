import React from 'react';
import { FormControlLabel, Switch } from '@mui/material';

export interface ToggleRowProps {
  /** Toggle label. */
  label: string;
  /** Controlled checked state. */
  checked: boolean;
  /** Change handler. */
  onChange?: (checked: boolean) => void;
  /** Disable the toggle. */
  disabled?: boolean;
}

/**
 * A labelled switch row for boolean settings.
 * @category Settings
 */
export const ToggleRow: React.FC<ToggleRowProps> = ({ label, checked, onChange, disabled }) => (
  <FormControlLabel
    control={
      <Switch checked={checked} disabled={disabled} onChange={(e) => onChange?.(e.target.checked)} />
    }
    label={label}
  />
);
