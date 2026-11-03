/**
 * ModuleFirmwareDialog — update an STD module's firmware through the hub, over Wi-Fi (root
 * 534ded9e). The user picks the `.ota` image from the module's maker; the dialog checks the
 * name and size, sends it to the bank that is not running, then follows the hub's progress
 * (writing ≈ 2 min → commit → activation ~10–15 s off the bus → verdict). Metering keeps going
 * while it writes; if the new image doesn't come up the module returns to the old one by itself.
 */
import React, { useEffect, useRef, useState } from 'react';
import {
  Dialog, DialogTitle, DialogContent, DialogActions, Button, Stack, Alert, Typography, LinearProgress, Link,
} from '@mui/material';
import { getApiClient } from '../../services/api';
import { SensorModule, ModuleFwProgress } from '../../types/sensors';
import { fwLabel, targetBank, fileProblem, fwPercent, fwRunning, sha256Hex, parseManifest, manifestProblem, manifestBankAddr, FwManifest } from './moduleFw';
import { t } from '../../i18n';

const kb = (n: number) => `${Math.round(n / 1024)} KB`;

export const ModuleFirmwareDialog: React.FC<{ module: SensorModule | null; onClose: () => void; onDone: () => void }> = ({ module: m, onClose, onDone }) => {
  const [file, setFile] = useState<File | null>(null);
  const [mf, setMf] = useState<{ name: string; data: FwManifest } | null>(null);
  const [sending, setSending] = useState(false);
  const [refusal, setRefusal] = useState<string | null>(null);
  const [prog, setProg] = useState<ModuleFwProgress | null>(null);
  const [lost, setLost] = useState(false);
  const inputRef = useRef<HTMLInputElement>(null);
  const mfRef = useRef<HTMLInputElement>(null);
  // the parent re-renders with telemetry; a new onDone must not restart the 1 s timer
  const doneRef = useRef(onDone);
  doneRef.current = onDone;
  const ota = m?.ota;
  const bank = targetBank(ota);

  // Reset on open; pick up an update already running (another tab, a reload mid-write).
  useEffect(() => {
    setFile(null); setMf(null); setRefusal(null); setProg(null); setLost(false);
    if (!m) return;
    getApiClient().getModuleFirmware(m.module).then((p) => { if (fwRunning(p)) setProg(p); }).catch(() => undefined);
  }, [m]);

  // 1 Hz while running; a missed answer during activation is expected (the hub is busy) — say so, keep polling.
  useEffect(() => {
    if (!m || !fwRunning(prog)) return undefined;
    const id = setTimeout(() => {
      getApiClient().getModuleFirmware(m.module)
        .then((p) => { setLost(false); setProg(p); if (p.state === 'done') doneRef.current(); })
        .catch(() => { setLost(true); setProg((p) => (p ? { ...p } : p)); });
    }, 1000);
    return () => clearTimeout(id);
  }, [m, prog]);

  if (!m) return null;
  const problem = file && ota ? fileProblem(file, ota) : null;
  // The sealed image can't be read; its .manifest says which bank and board it was built for
  // (root 9bc08b40) — a mismatch is refused here, before ~2 min of writing.
  const mfProblem = mf && ota ? manifestProblem(mf.data, ota, bank, file?.name) : null;
  const mfBank = mf && ota ? manifestBankAddr(mf.data, ota) : null;
  const mfText = mf ? [
    mf.data.version ? t('modfw.mfVersion', { fw: fwLabel({ fw: mf.data.version, line: 'std' }) ?? '' }) : '',
    mf.data.variant ?? '',
    mf.data.bank ? t('modfw.bank', { bank: mfBank && mfBank !== mf.data.bank ? `${mf.data.bank} (${mfBank})` : mf.data.bank }) : '',
    mf.data.board ? t('modfw.board', { board: mf.data.board }) : '',
  ].filter(Boolean).join(' · ') : '';
  const sameVersion = !!mf?.data.version && mf.data.version === m?.fw;
  const running = fwRunning(prog);
  const finished = prog?.state === 'done' || prog?.state === 'error';

  const start = async () => {
    if (!file || !ota || problem || mfProblem) return;
    setSending(true); setRefusal(null);
    try {
      const buf = await file.arrayBuffer();
      const sha256 = (await sha256Hex(buf)) ?? mf?.data.sha256;
      // bank: the hub's inactive one (it would pick the same without it); board from the manifest
      // lets the hub refuse a wrong board before writing, not at commit.
      const r = await getApiClient().startModuleFirmware(m.module, buf, {
        ...(bank ? { bank } : {}), ...(mf?.data.board ? { board: mf.data.board } : {}), ...(sha256 ? { sha256 } : {}),
      });
      if (r.status === 'started') setProg({ module: m.module, state: 'writing', done: 0, total: Math.ceil(ota.bank_size / 256), fw_before: m.fw });
      else setRefusal(r.http === 409 ? t('modfw.busy') : t('modfw.refused', { why: [r.reason, r.message].filter(Boolean).join(': ') || String(r.http ?? '') }));
    } catch (e) {
      setRefusal(t('modfw.failed', { why: e instanceof Error ? e.message : String(e) }));
    } finally { setSending(false); }
  };

  const stepText = prog ? t(`modfw.state_${prog.state}`) : '';
  return (
    <Dialog open onClose={sending ? undefined : onClose} fullWidth maxWidth="sm">
      <DialogTitle>{t('modfw.title', { model: m.model ?? t('devices.moduleN', { module: m.module }) })}</DialogTitle>
      <DialogContent>
        <Stack spacing={1.5} sx={{ pt: 0.5 }}>
          {prog?.state !== 'done' && <Typography variant="body2">{t('modfw.current', { fw: fwLabel(m) ?? '—' })}</Typography>}
          {!prog && (
            <>
              <Typography variant="body2" color="text.secondary">{t('modfw.intro')}</Typography>
              {ota && (ota.board || bank) && (
                <Typography variant="caption" color="text.secondary" sx={{ fontVariantNumeric: 'tabular-nums' }}>
                  {[ota.board ? t('modfw.board', { board: ota.board }) : '', bank ? t('modfw.bank', { bank }) : ''].filter(Boolean).join(' · ')}
                </Typography>
              )}
              <input ref={inputRef} type="file" accept=".ota" hidden onChange={(e) => { setFile(e.target.files?.[0] ?? null); setRefusal(null); }} />
              <Stack direction="row" spacing={1} alignItems="center" flexWrap="wrap" useFlexGap>
                <Button variant="outlined" onClick={() => inputRef.current?.click()} disabled={sending}>{t(file ? 'modfw.otherFile' : 'modfw.chooseFile')}</Button>
                {file && <Typography variant="body2" sx={{ fontVariantNumeric: 'tabular-nums' }}>{file.name} · {kb(file.size)}</Typography>}
              </Stack>
              <input ref={mfRef} type="file" accept=".manifest,.txt,.json" hidden onChange={async (e) => {
                const f = e.target.files?.[0];
                setMf(f ? { name: f.name, data: parseManifest(await f.text()) } : null);
              }} />
              <Stack direction="row" spacing={1} alignItems="center" flexWrap="wrap" useFlexGap>
                <Button size="small" onClick={() => mfRef.current?.click()} disabled={sending}>{t(mf ? 'modfw.otherManifest' : 'modfw.chooseManifest')}</Button>
                {mf && <Typography variant="body2" sx={{ fontVariantNumeric: 'tabular-nums' }}>
                  {mf.name} · {mfText || t('modfw.manifestEmpty')}
                </Typography>}
              </Stack>
              {!mf && file && <Typography variant="caption" color="text.secondary">{t('modfw.noManifest')}</Typography>}
              {mfProblem === 'file' && <Alert severity="error">{t('modfw.wrongFile', { got: file!.name, want: mf!.data.file! })}</Alert>}
              {mfProblem === 'bank' && <Alert severity="error">{t('modfw.wrongBank', { got: mfBank ? `${mf!.data.bank} (${mfBank})` : mf!.data.bank!, want: bank! })}</Alert>}
              {sameVersion && !mfProblem && <Alert severity="info">{t('modfw.sameVersion')}</Alert>}
              {mfProblem === 'board' && <Alert severity="error">{t('modfw.wrongBoard', { got: mf!.data.board!, want: ota!.board! })}</Alert>}
              {problem === 'ext' && <Alert severity="error">{t('modfw.badExt')}</Alert>}
              {problem === 'size' && ota && <Alert severity="error">{t('modfw.badSize', { got: file!.size, want: ota.bank_size })}</Alert>}
              {file && !problem && !mfProblem && <Alert severity="info">{t('modfw.before')}</Alert>}
            </>
          )}
          {prog && (
            <>
              <Typography variant="body2" fontWeight={600}>{stepText}</Typography>
              <LinearProgress variant="determinate" value={fwPercent(prog)} color={prog.state === 'error' ? 'error' : prog.state === 'done' ? 'success' : 'primary'} />
              <Typography variant="caption" color="text.secondary" sx={{ fontVariantNumeric: 'tabular-nums' }}>
                {[prog.total ? t('modfw.pages', { done: prog.done ?? 0, total: prog.total }) : '', prog.elapsed_s != null ? t('modfw.elapsed', { s: prog.elapsed_s }) : ''].filter(Boolean).join(' · ')}
              </Typography>
              {running && <Typography variant="body2" color="text.secondary">{t(prog.state === 'activating' ? 'modfw.whileActivating' : 'modfw.whileWriting')}</Typography>}
              {lost && running && <Typography variant="caption" color="text.secondary">{t('modfw.noAnswer')}</Typography>}
              {prog.state === 'done' && (
                <Alert severity="success">{t('modfw.done', { before: fwLabel({ fw: prog.fw_before, line: m.line }) ?? '—', after: fwLabel({ fw: prog.fw_after, line: m.line }) ?? '—' })}</Alert>
              )}
              {prog.state === 'error' && (
                <Alert severity="error">
                  {t('modfw.error', { why: [prog.reason, prog.message].filter(Boolean).join(': ') || '—' })}{' '}
                  <Link href="#/events" underline="hover">{t('modfw.log')}</Link>
                </Alert>
              )}
            </>
          )}
          {refusal && <Alert severity="error">{refusal}</Alert>}
        </Stack>
      </DialogContent>
      <DialogActions>
        {finished || !prog
          ? <Button onClick={onClose} disabled={sending}>{t(finished ? 'common.close' : 'common.cancel')}</Button>
          // closing doesn't stop the hub; reopening picks the update up again
          : <Button onClick={onClose}>{t('modfw.hide')}</Button>}
        {!prog && (
          <Button variant="contained" onClick={start} disabled={!file || !!problem || !!mfProblem || sending}>
            {sending ? t('modfw.sending') : t('modfw.start')}
          </Button>
        )}
      </DialogActions>
    </Dialog>
  );
};
