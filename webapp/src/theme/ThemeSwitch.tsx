/**
 * ThemeSwitch — theme: system / dark / light (phrases shell.theme*). Compact 3-way toggle for the shell
 * (rail footer on desktop, «Ещё» drawer on mobile). Persists via ThemeProvider.setPreference.
 */
import React from 'react';
import { Box, ToggleButton, ToggleButtonGroup, Typography } from '@mui/material';
import BrightnessAutoIcon from '@mui/icons-material/BrightnessAuto';
import DarkModeIcon from '@mui/icons-material/DarkMode';
import LightModeIcon from '@mui/icons-material/LightMode';
import { useTheme, ThemePreference } from './ThemeProvider';
import { t } from '../i18n';

const OPTIONS: { value: ThemePreference; label: string; icon: React.ReactNode }[] = [
  { value: 'system', label: 'shell.themeSystem', icon: <BrightnessAutoIcon fontSize="small" /> },
  { value: 'dark', label: 'shell.themeDark', icon: <DarkModeIcon fontSize="small" /> },
  { value: 'light', label: 'shell.themeLight', icon: <LightModeIcon fontSize="small" /> },
];

export const ThemeSwitch: React.FC = () => {
  const { preference, setPreference, forced } = useTheme();
  const current = OPTIONS.find((o) => o.value === preference) ?? OPTIONS[0];
  return (
    <Box sx={{ px: 2, py: 1.5 }}>
      <Typography id="rbgrid-theme-label" variant="caption" color="text.secondary" display="block" sx={{ mb: 0.75 }}>
        {t('shell.themeIs', { theme: t(current.label) })}
      </Typography>
      <ToggleButtonGroup
        size="small"
        exclusive
        fullWidth
        disabled={forced}
        value={preference}
        onChange={(_, v: ThemePreference | null) => v && setPreference(v)}
        aria-labelledby="rbgrid-theme-label"
      >
        {OPTIONS.map((o) => (
          <ToggleButton key={o.value} value={o.value} aria-label={t('shell.themeIs', { theme: t(o.label) })} title={t('shell.themeIs', { theme: t(o.label) })} sx={{ py: 0.5 }}>
            {o.icon}
          </ToggleButton>
        ))}
      </ToggleButtonGroup>
    </Box>
  );
};
