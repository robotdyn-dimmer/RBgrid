/**
 * AddModuleDialog — add an rbAmp module on bus 0 (v1.1.2, operator 02.11 via root: «up to 3
 * modules»). The step comes from the hub's state (addModule.addStep): free the factory address
 * 0x50 by moving the module there → connect the new one → scan → add. Closing for a power-off
 * is fine: reopening picks up where the hub is.
 */
import React, { useCallback, useEffect, useState } from 'react';
import {
  Dialog, DialogTitle, DialogContent, DialogActions, Button, Stack, Alert, Typography, CircularProgress, Link,
} from '@mui/material';
import { getApiClient, ApiRequestError } from '../../services/api';
import type { I2cDevice, I2cState } from '../../types/i2c';
import { addStep, newModules, otherDevices, pullupKey, hex, maxModules, BUS } from './addModule';
import { t } from '../../i18n';

/** rbAmp wiring guide (longer cables and other cases) — the rbamp.com page the site links too. */
export const PULLUP_DOC = 'https://www.rbamp.com/docs/modules-basic-standard-hardware-connection';

const why = (e: unknown): string => (e instanceof Error ? e.message : String(e));

export const AddModuleDialog: React.FC<{ open: boolean; onClose: () => void; onChanged: () => void }> = ({ open, onClose, onChanged }) => {
  const [state, setState] = useState<I2cState | null>(null);
  const [loadErr, setLoadErr] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState<string | null>(null);
  const [scan, setScan] = useState<I2cDevice[] | null>(null);
  const [added, setAdded] = useState<{ module: number; addr: number } | null>(null);
  const [moved, setMoved] = useState<{ module: number; addr: number } | null>(null);

  const load = useCallback(async () => {
    setLoadErr(null);
    try { setState(await getApiClient().getI2c()); } catch (e) {
      setLoadErr(e instanceof ApiRequestError && e.status === 404 ? t('addmod.noApi') : t('addmod.loadFailed', { why: why(e) }));
    }
  }, []);

  useEffect(() => {
    if (!open) return;
    setState(null); setErr(null); setScan(null); setAdded(null); setMoved(null);
    void load();
  }, [open, load]);

  const run = async (f: () => Promise<void>) => {
    setBusy(true); setErr(null);
    try { await f(); } catch (e) { setErr(t('addmod.failed', { why: why(e) })); } finally { setBusy(false); }
  };

  const move = (module: number, to: number) => run(async () => {
    const r = await getApiClient().setI2cModuleAddr(module, to);
    if (r.status !== 'ok') { setErr(t('addmod.refused', { why: r.message })); return; }
    setState(r); setScan(null); setMoved({ module, addr: to }); onChanged();
  });

  const doScan = () => run(async () => {
    setScan(null);
    const r = await getApiClient().scanI2c(BUS);
    if (r.status !== 'ok') { setErr(r.code === 'BUS_STUCK' ? t('addmod.busStuck') : t('addmod.refused', { why: r.message })); return; }
    setScan(r.devices);
  });

  const add = (addr: number) => run(async () => {
    const r = await getApiClient().addI2cModule(BUS, addr);
    if (r.status !== 'ok') { setErr(t('addmod.refused', { why: r.message })); return; }
    const m = r.modules.find((x) => x.bus === BUS && x.addr === addr);
    setState(r); setScan(null); setMoved(null); setAdded({ module: m?.module ?? r.modules.length - 1, addr }); onChanged();
  });

  const another = () => { setAdded(null); setScan(null); setErr(null); };

  const step = state ? addStep(state) : null;
  const found = scan ? newModules(scan) : [];
  const others = scan ? otherDevices(scan) : [];
  const pull = step?.kind === 'connect' ? pullupKey(step.count + 1) : null;

  let body: React.ReactNode;
  if (loadErr) body = <Alert severity="error">{loadErr}</Alert>;
  else if (!state || !step) body = <Stack alignItems="center" py={3}><CircularProgress aria-label={t('common.loading')} /></Stack>;
  else if (added) {
    body = (
      <Stack spacing={1.5}>
        <Alert severity="success">{t('addmod.added', { module: added.module, addr: hex(added.addr) })}</Alert>
        <Typography variant="body2">{t('addmod.addedNext')}</Typography>
      </Stack>
    );
  } else if (step.kind === 'full') {
    body = <Alert severity="info">{t('addmod.full', { max: step.max })}</Alert>;
  } else if (step.kind === 'free') {
    body = (
      <Stack spacing={1.5}>
        <Typography variant="body2">{t('addmod.whyFree')}</Typography>
        {/* A module already set to another address doesn't need 0x50 (root 03.11): scan finds it. */}
        {found.map((d) => (
          <Stack key={d.addr} direction="row" alignItems="center" spacing={1.5} flexWrap="wrap">
            <Typography sx={{ fontVariantNumeric: 'tabular-nums' }}>{t('addmod.foundModule', { addr: hex(d.addr), model: d.model ?? 'rbAmp', fw: d.fw ?? '—' })}</Typography>
            <Button variant="contained" size="small" disabled={busy} onClick={() => add(d.addr)}>{t('addmod.add')}</Button>
          </Stack>
        ))}
        {scan && found.length === 0 && <Typography variant="body2" color="text.secondary">{t('addmod.noneElsewhere')}</Typography>}
        {!step.present ? <Alert severity="warning">{t('addmod.freeAbsent', { module: step.module })}</Alert>
          : step.to == null ? <Alert severity="warning">{t('addmod.noFreeAddr')}</Alert>
            : <Typography variant="body2">{t('addmod.freeWhat', { module: step.module, model: step.model ?? '', to: hex(step.to) })}</Typography>}
      </Stack>
    );
  } else {
    body = (
      <Stack spacing={1.5}>
        {moved && <Alert severity="success">{t('addmod.moved', { module: moved.module, addr: hex(moved.addr) })}</Alert>}
        <Typography variant="body2" component="div">
          <ol style={{ margin: 0, paddingLeft: 20 }}>
            <li>{t('addmod.step1')} {t('addmod.step1Unplug')}</li>
            <li>{t('addmod.step2')}</li>
            <li>{t('addmod.step3')}</li>
          </ol>
        </Typography>
        {pull && (
          <Alert severity="info">
            {t(pull)} <Link href={PULLUP_DOC} target="_blank" rel="noopener noreferrer">{t('addmod.pullupLink')}</Link>
          </Alert>
        )}
        {scan && found.length === 0 && <Alert severity="warning">{t('addmod.noneFound')}</Alert>}
        {found.map((d) => (
          <Stack key={d.addr} direction="row" alignItems="center" spacing={1.5} flexWrap="wrap">
            <Typography sx={{ fontVariantNumeric: 'tabular-nums' }}>{t('addmod.foundModule', { addr: hex(d.addr), model: d.model ?? 'rbAmp', fw: d.fw ?? '—' })}</Typography>
            <Button variant="contained" size="small" disabled={busy} onClick={() => add(d.addr)}>{t('addmod.add')}</Button>
          </Stack>
        ))}
        {others.length > 0 && (
          <Typography variant="caption" color="text.secondary" sx={{ fontVariantNumeric: 'tabular-nums' }}>
            {t('addmod.others', { list: others.map((d) => hex(d.addr)).join(', ') })}
          </Typography>
        )}
      </Stack>
    );
  }

  const canAnother = !!(added && state && state.modules.length < maxModules(state));
  return (
    <Dialog open={open} onClose={busy ? undefined : onClose} maxWidth="sm" fullWidth>
      <DialogTitle>{t('addmod.title')}</DialogTitle>
      <DialogContent>
        <Stack spacing={1.5}>
          {body}
          {err && <Alert severity="error" onClose={() => setErr(null)}>{err}</Alert>}
        </Stack>
      </DialogContent>
      <DialogActions>
        <Button onClick={onClose} disabled={busy}>{added || step?.kind === 'full' ? t('addmod.done') : t('addmod.cancel')}</Button>
        {canAnother && <Button onClick={another} disabled={busy}>{t('addmod.another')}</Button>}
        {!added && step?.kind === 'free' && step.present && step.to != null && (
          <Button variant="contained" disabled={busy} onClick={() => move(step.module, step.to as number)}>
            {busy ? t('addmod.moving') : t('addmod.move', { to: hex(step.to) })}
          </Button>
        )}
        {!added && step?.kind === 'free' && !step.present && <Button onClick={() => void load()} disabled={busy}>{t('addmod.recheck')}</Button>}
        {!added && step?.kind === 'free' && <Button disabled={busy} onClick={doScan}>{t('addmod.scanOther')}</Button>}
        {!added && step?.kind === 'connect' && (
          <Button variant={found.length ? 'outlined' : 'contained'} disabled={busy} onClick={doScan}>
            {busy ? t('addmod.scanning') : scan ? t('addmod.rescan') : t('addmod.scan')}
          </Button>
        )}
      </DialogActions>
    </Dialog>
  );
};
