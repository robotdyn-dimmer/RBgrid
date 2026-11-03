/**
 * ConfirmDialog — the app's one confirmation for consequential actions (unbind / delete a point,
 * reboot, factory reset, token rotation, Wi-Fi switch). Replaces native window.confirm (UX audit
 * HEU-10 / PNT-17): in the design system, names the object, says what happens, and the
 * destructive button is the red one.
 *
 *   const confirm = useConfirm();
 *   if (!(await confirm({ title, body, confirmLabel, danger: true }))) return;
 */
import React, { createContext, useCallback, useContext, useRef, useState } from 'react';
import { Dialog, DialogTitle, DialogContent, DialogContentText, DialogActions, Button } from '@mui/material';
import { t } from '../../i18n';

export interface ConfirmOptions {
  title: string;
  /** What will happen (consequences), one or two sentences. */
  body?: React.ReactNode;
  /** Label of the confirming button (default «Подтвердить»). */
  confirmLabel?: string;
  /** Destructive action: red confirm button. */
  danger?: boolean;
  /** Label of the cancelling button (default «Отмена»). */
  cancelLabel?: string;
}

type ConfirmFn = (o: ConfirmOptions) => Promise<boolean>;

const Ctx = createContext<ConfirmFn | null>(null);

export const ConfirmProvider: React.FC<{ children: React.ReactNode }> = ({ children }) => {
  const [opts, setOpts] = useState<ConfirmOptions | null>(null);
  const resolver = useRef<((v: boolean) => void) | null>(null);

  const confirm = useCallback<ConfirmFn>((o) => new Promise<boolean>((resolve) => {
    resolver.current?.(false);
    resolver.current = resolve;
    setOpts(o);
  }), []);

  const close = (v: boolean) => {
    resolver.current?.(v);
    resolver.current = null;
    setOpts(null);
  };

  return (
    <Ctx.Provider value={confirm}>
      {children}
      <Dialog open={!!opts} onClose={() => close(false)} maxWidth="xs" fullWidth aria-labelledby="confirm-title">
        {opts && (
          <>
            <DialogTitle id="confirm-title">{opts.title}</DialogTitle>
            {opts.body && (
              <DialogContent>
                <DialogContentText component="div">{opts.body}</DialogContentText>
              </DialogContent>
            )}
            <DialogActions>
              <Button onClick={() => close(false)} autoFocus>{opts?.cancelLabel ?? t('common.cancel')}</Button>
              <Button onClick={() => close(true)} variant="contained" color={opts.danger ? 'error' : 'primary'}>
                {opts.confirmLabel ?? t('common.confirm')}
              </Button>
            </DialogActions>
          </>
        )}
      </Dialog>
    </Ctx.Provider>
  );
};

/** Confirmation prompt; outside a provider (tests) it falls back to "confirmed". */
export function useConfirm(): ConfirmFn {
  return useContext(Ctx) ?? (async () => true);
}
