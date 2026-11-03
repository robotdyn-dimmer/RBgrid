/**
 * EvPanel — «Зарядные станции»: system.ev {charge_threshold_w, idle_timeout_min, price_per_kwh}
 * (gap G7, live). Moved out of «Система» (operator 18.10): it is the EV scenario's settings, so it
 * shows only on the EV scenario or when a point has the EV_CHARGING role. One value per hub
 * (root W4: one sell price per hub; per-station = phase 2). A per-station threshold / idle
 * override is asked of root — then it lands in the point's parameters.
 * Saved with «Применить» (system-config, live).
 */
import React from 'react';
import { Box, Typography } from '@mui/material';
import { PanelCard } from '@rbgrid/design-system';
import { useConfigShadow } from '../../core';
import { useSiteProfile } from '../../hooks/useSiteProfile';
import { useUnitsConfig } from '../../hooks/useUnitsConfig';
import { ShadowConfigEditor } from './ShadowConfigEditor';
import { t } from '../../i18n';

export const EvPanel: React.FC<{ anchorMargin?: number }> = ({ anchorMargin = 88 }) => {
  const [profile] = useSiteProfile();
  const sys = useConfigShadow('system');
  useUnitsConfig(); // loads units-config (roles)
  const units = useConfigShadow('units');
  const list = Array.isArray(units.reported.units) ? (units.reported.units as { role?: unknown }[]) : [];
  const stations = list.some((u) => u.role === 'EV_CHARGING');
  if (!sys.desired.ev || (profile !== 'ev' && !stations)) return null;
  return (
    <Box id="ev" sx={{ scrollMarginTop: anchorMargin }}>
      <PanelCard title={t('evPanel.title')}>
        <Typography variant="body2" color="text.secondary" sx={{ mb: 1.5 }}>{t('evPanel.intro')}</Typography>
        <ShadowConfigEditor domain="system" only={['ev']} />
        <Typography variant="caption" color="text.secondary" sx={{ display: 'block', mt: 1.5 }}>{t('evPanel.note')}</Typography>
      </PanelCard>
    </Box>
  );
};
