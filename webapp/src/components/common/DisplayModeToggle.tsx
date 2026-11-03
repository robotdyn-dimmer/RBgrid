/**
 * DisplayModeToggle — the «Мощность | Деньги» segment for the Overview header (view lens;
 * not a device setting). Hidden when the device has no active tariff (money unavailable).
 */
import React from 'react';
import { ToggleButton, ToggleButtonGroup } from '@mui/material';
import { DisplayMode } from '../../hooks/useDisplayMode';
import { t } from '../../i18n';

export const DisplayModeToggle: React.FC<{
  mode: DisplayMode;
  available: boolean;
  onChange: (m: DisplayMode) => void;
  fullWidthOnPhone?: boolean;
}> = ({ mode, available, onChange, fullWidthOnPhone }) => {
  if (!available) return null;
  return (
    <ToggleButtonGroup
      size="small"
      exclusive
      value={mode}
      onChange={(_, v) => v && onChange(v)}
      aria-label={t('displayMode.aria')}
      sx={fullWidthOnPhone ? { flex: { xs: '1 1 100%', md: '0 0 auto' }, '& .MuiToggleButton-root': { flex: { xs: 1, md: 'initial' } } } : undefined}
    >
      <ToggleButton value="power">{t('displayMode.power')}</ToggleButton>
      <ToggleButton value="money">{t('displayMode.money')}</ToggleButton>
    </ToggleButtonGroup>
  );
};
