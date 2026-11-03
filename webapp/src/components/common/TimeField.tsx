/**
 * TimeField — «ЧЧ:ММ», 24-hour, whatever the browser locale (a native type="time" showed
 * «11:00 PM» to a Russian reader). Commits on a valid value; shows the reason otherwise.
 */
import React, { useEffect, useState } from 'react';
import { TextField } from '@mui/material';
import { t } from '../../i18n';
import { hhmm, parseHhmm } from '../../economics/zoneCoverage';

export const TimeField: React.FC<{ label: string; minutes: number; onChange: (min: number) => void; onError?: (err: string | null) => void }> = ({ label, minutes, onChange, onError }) => {
  const [text, setText] = useState(hhmm(minutes));
  useEffect(() => { setText(hhmm(minutes)); }, [minutes]);
  const parsed = parseHhmm(text);
  const bad = parsed == null || parsed >= 1440;
  // An invalid text blocks the form's Save (rerun 16.10 TAR-R07: «25:00» left the old time in place
  // and Save went ahead with it).
  useEffect(() => { onError?.(bad ? t('form.timeFormat') : null); }, [bad]); // eslint-disable-line react-hooks/exhaustive-deps
  return (
    <TextField
      size="small" label={label} value={text} placeholder="ЧЧ:ММ" sx={{ flex: 1 }}
      inputProps={{ inputMode: 'numeric', maxLength: 5 }}
      error={bad} helperText={bad ? t('form.timeFormat') : undefined}
      onChange={(e) => {
        let v = e.target.value.replace(/[^\d:]/g, '');
        if (/^\d{3,4}$/.test(v)) v = `${v.slice(0, v.length - 2)}:${v.slice(-2)}`;
        setText(v);
        const m = parseHhmm(v);
        if (m != null && m < 1440) onChange(m);
      }}
    />
  );
};
