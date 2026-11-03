/**
 * RebootNowButton — «Перезагрузить сейчас» next to a saved-but-needs-restart message (time,
 * node pairing…), so a write the hub applies only at start never ends on a bare note
 * (save-split audit 18.10). Same confirm as «Администрирование → Перезагрузить».
 */
import React, { useState } from 'react';
import { Button } from '@mui/material';
import { getApiClient } from '../../services/api';
import { useConfirm } from './ConfirmDialog';
import { t } from '../../i18n';

export const RebootNowButton: React.FC<{ onDone?: (ok: boolean) => void }> = ({ onDone }) => {
  const confirm = useConfirm();
  const [state, setState] = useState<'idle' | 'busy' | 'sent'>('idle');
  const go = async () => {
    if (!(await confirm({ title: t('admin.rebootConfirm'), body: t('admin.rebootBody'), confirmLabel: t('admin.rebootAction') }))) return;
    setState('busy');
    try {
      await getApiClient().rebootDevice();
      setState('sent');
      onDone?.(true);
    } catch {
      setState('idle');
      onDone?.(false);
    }
  };
  return (
    <Button color="inherit" size="small" onClick={go} disabled={state !== 'idle'}>
      {state === 'idle' ? t('reconcile.rebootNow') : state === 'busy' ? t('reconcile.rebootingShort') : t('reconcile.rebootSent')}
    </Button>
  );
};
