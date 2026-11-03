/**
 * SetupBanner — a fresh hub opens straight on the Overview with money counted by the factory
 * sample tariff and a site called «Default»; nothing pointed at the setup wizard (fresh hub 17.10,
 * W191-11). Shown while the factory signs remain; says which, links the wizard, can be hidden.
 * The time zone has its own note (HubTimeNote).
 */
import React, { useEffect, useState } from 'react';
import { Alert, Button, Stack } from '@mui/material';
import { Link as RouterLink } from 'react-router-dom';
import { getApiClient } from '../../services/api';
import { useLocalStorage } from '../../hooks/useLocalStorage';
import { t } from '../../i18n';

export const SetupBanner: React.FC<{ placeholderTariff: boolean }> = ({ placeholderTariff }) => {
  const [hidden, setHidden] = useLocalStorage<boolean>('rbgrid.setupBanner.hidden', false);
  const [unnamed, setUnnamed] = useState(false);

  useEffect(() => {
    let on = true;
    getApiClient().getSystemSettings()
      .then((s) => { const n = (s as unknown as { home_name?: string }).home_name; if (on) setUnnamed(!n || n === 'Default'); })
      .catch(() => { /* offline — say nothing */ });
    return () => { on = false; };
  }, []);

  const missing = [...(unnamed ? [t('setupBanner.name')] : []), ...(placeholderTariff ? [t('setupBanner.tariff')] : [])];
  if (hidden || !missing.length) return null;
  return (
    <Alert severity="info" sx={{ mb: 2 }}
      action={
        <Stack direction="row" spacing={0.5} alignItems="center">
          <Button color="inherit" size="small" variant="outlined" component={RouterLink} to="/onboarding" sx={{ whiteSpace: 'nowrap' }}>{t('setupBanner.go')}</Button>
          <Button color="inherit" size="small" onClick={() => setHidden(true)}>{t('setupBanner.hide')}</Button>
        </Stack>
      }>
      {t('setupBanner.text', { what: missing.join(', ') })}
    </Alert>
  );
};
