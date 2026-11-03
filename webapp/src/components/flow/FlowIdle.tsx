/** The idle state of the «Распределение» / «Баланс» schemas: nothing flows → a sentence, not empty axes. */
import React from 'react';
import { Typography } from '@mui/material';
import { t } from '../../i18n';

export const FlowIdle: React.FC = () => (
  <Typography color="text.secondary" sx={{ fontSize: 14, py: 3, textAlign: 'center' }}>{t('flow.idleNow')}</Typography>
);
