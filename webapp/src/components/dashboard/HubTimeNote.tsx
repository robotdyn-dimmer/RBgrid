/**
 * HubTimeNote — every time on the screens is the HUB's clock; when its zone differs from this
 * browser's, say so once on the Overview (fresh hub 17.10, W191-13: «06:14» in Berlin at 08:14,
 * with no hint the hub was still on UTC — the day, the hours and the tariff zones all shift).
 */
import React from 'react';
import { Alert, Link } from '@mui/material';
import { useDeviceClock } from '../../core/deviceTime';
import { t } from '../../i18n';

const utcLabel = (min: number) => {
  const sign = min < 0 ? '−' : '+';
  const a = Math.abs(min), h = Math.floor(a / 60), m = a % 60;
  return `UTC${sign}${h}${m ? `:${String(m).padStart(2, '0')}` : ''}`;
};

export const HubTimeNote: React.FC = () => {
  const clock = useDeviceClock();
  if (!clock.known) return null;
  const now = Math.floor(Date.now() / 1000);
  const p = clock.parts(now);
  const hubOff = Math.round((Date.UTC(p.y, p.mo - 1, p.d, p.h, p.mi) / 1000 - (now - (now % 60))) / 60);
  const myOff = -new Date().getTimezoneOffset();
  if (Math.abs(hubOff - myOff) < 30) return null;
  return (
    <Alert severity="info" sx={{ mb: 2 }}>
      {t('hubTime.differs', { hub: utcLabel(hubOff), mine: utcLabel(myOff) })}{' '}
      <Link href="#/config#time" underline="hover">{t('hubTime.change')}</Link>
    </Alert>
  );
};
