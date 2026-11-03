/**
 * DecimalField — a number input that accepts the reader's decimal separator (UX audit TAR-04: a
 * `type=number` field dropped the Russian comma, «6,43» became 643 and was saved). Text input
 * with `inputmode=decimal`; «,» and «.» both parse; shown in the UI locale; range / decimals /
 * required checked inline with the reason under the field. The parent gets (value, error) and
 * blocks Save while any error stands.
 */
import React, { useEffect, useRef, useState } from 'react';
import { TextField } from '@mui/material';
import { t, intlLocale } from '../../i18n';

export interface DecimalRules {
  min?: number;
  max?: number;
  /** Max digits after the separator. */
  decimals?: number;
  /** Empty is an error (default true). */
  required?: boolean;
  /** 0 is accepted outside [min, max] — «not set» (hub `zero_unset`). */
  allowZero?: boolean;
}

/** Parse a user-typed decimal ("6,43", "6.43", " 1 200,5"); null = empty; NaN = not a number. */
export function parseDecimal(text: string): number | null {
  const s = text.replace(/[\s  ]/g, '').replace(',', '.');
  if (s === '') return null;
  if (!/^-?\d*\.?\d*$/.test(s) || s === '-' || s === '.' || s === '-.') return NaN;
  return Number(s);
}

/** Validation message for a typed value, or null when it's fine. */
export function decimalError(text: string, rules: DecimalRules): string | null {
  const v = parseDecimal(text);
  if (v === null) return rules.required === false ? null : t('form.required');
  if (Number.isNaN(v)) return t('form.notNumber');
  if (v === 0 && rules.allowZero) return null;
  if (rules.min !== undefined && v < rules.min) return t('form.min', { min: fmt(rules.min) });
  if (rules.max !== undefined && v > rules.max) return t('form.max', { max: fmt(rules.max) });
  const frac = text.replace(',', '.').split('.')[1]?.replace(/\s/g, '') ?? '';
  if (rules.decimals !== undefined && frac.length > rules.decimals) return t('form.decimals', { n: rules.decimals });
  return null;
}

const fmt = (n: number): string => n.toLocaleString(intlLocale(), { maximumFractionDigits: 6, useGrouping: false });

export interface DecimalFieldProps extends DecimalRules {
  label: string;
  value: number | null | undefined;
  onChange: (value: number | null, error: string | null) => void;
  /** Unit / hint under the field when there is no error. */
  helper?: React.ReactNode;
  disabled?: boolean;
}

export const DecimalField: React.FC<DecimalFieldProps> = ({
  label, value, onChange, helper, disabled, ...rules
}) => {
  const [text, setText] = useState(value == null ? '' : fmt(value));
  const [err, setErr] = useState<string | null>(null);
  const focused = useRef(false);

  // An outside change (re-read after save, reset) replaces the text unless the user is typing.
  useEffect(() => {
    if (!focused.current) { setText(value == null ? '' : fmt(value)); setErr(null); }
  }, [value]);

  return (
    <TextField
      size="small" fullWidth label={label} value={text} disabled={disabled}
      required={rules.required !== false}
      error={!!err} helperText={err ?? helper}
      inputProps={{ inputMode: 'decimal', autoComplete: 'off' }}
      onFocus={() => { focused.current = true; }}
      onBlur={() => { focused.current = false; }}
      onChange={(e) => {
        const next = e.target.value;
        setText(next);
        const e2 = decimalError(next, rules);
        setErr(e2);
        const v = parseDecimal(next);
        onChange(e2 || v === null || Number.isNaN(v) ? null : v, e2);
      }}
    />
  );
};
