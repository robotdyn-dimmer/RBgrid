/**
 * A point the hub keeps no history for (v1.1.2 units-config `history:false`, root 6e32d29): live values,
 * HA / MQTT and today's counters work, hour/day/month records aren't stored. Why: the hub's history
 * slots are full, or the point was bound after the last restart (slots are set up at boot).
 * Older hubs don't send the flag → nothing shown.
 */
import React from 'react';
import { Alert } from '@mui/material';
import { t } from '../../i18n';

/** History slots of a v1.1.2 hub (operator 03.11 via root). */
export const HISTORY_SLOTS = 12;

export const NoHistoryNote: React.FC<{ history?: boolean; historyCount: number | null; name?: string }> = ({ history, historyCount, name }) => {
  if (history !== false) return null;
  const why = historyCount != null && historyCount >= HISTORY_SLOTS ? t('point.historyFull', { n: HISTORY_SLOTS }) : t('point.historyRestart');
  return (
    <Alert severity="info" sx={{ mb: 1.5 }}>
      {name ? t('point.liveOnlyNamed', { name }) : t('point.liveOnly')} {why}
    </Alert>
  );
};
