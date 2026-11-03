import React from 'react';
import { TextField, MenuItem } from '@mui/material';

export interface FormFieldOption {
  /** Option value. */
  value: string;
  /** Option label (defaults to value). */
  label?: string;
}

export interface FormFieldProps {
  /** Field label. */
  label: string;
  /** Controlled value. */
  value: string;
  /** Change handler. */
  onChange?: (value: string) => void;
  /** Input type (text inputs only). */
  type?: 'text' | 'password' | 'number';
  /** When set, renders a select with these options. */
  options?: FormFieldOption[];
  /** Mark required. */
  required?: boolean;
  /** Disable the field. */
  disabled?: boolean;
  /** Full width (default true). */
  fullWidth?: boolean;
  /** Placeholder text (text inputs only). */
  placeholder?: string;
  /** Focus the field on mount. */
  autoFocus?: boolean;
  /** Key handler — e.g. submit on Enter. */
  onKeyDown?: (e: React.KeyboardEvent<HTMLDivElement>) => void;
}

/**
 * A controlled text or select field — the building block of every settings form.
 * @category Settings
 */
export const FormField: React.FC<FormFieldProps> = ({
  label,
  value,
  onChange,
  type = 'text',
  options,
  required = false,
  disabled = false,
  fullWidth = true,
  placeholder,
  autoFocus = false,
  onKeyDown,
}) => (
  <TextField
    size="small"
    label={label}
    value={value}
    type={options ? undefined : type}
    select={!!options}
    required={required}
    disabled={disabled}
    fullWidth={fullWidth}
    placeholder={options ? undefined : placeholder}
    autoFocus={autoFocus}
    onKeyDown={onKeyDown}
    onChange={(e) => onChange?.(e.target.value)}
  >
    {options?.map((o) => (
      <MenuItem key={o.value} value={o.value}>
        {o.label ?? o.value}
      </MenuItem>
    ))}
  </TextField>
);
