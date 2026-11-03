/**
 * VampireHuntDialog — «Охота на вампиров» (documents/design/HOME_VAMPIRES.md, step 2).
 * Step by step on the input's LIVE power: 30 s of «as usual», the user switches ONE device off and
 * presses «Выключил», 30 s more after a 3 s settle → the step «−4,0 Вт (L2)» priced per month,
 * named and kept in a list (this browser, per hub — a note of the viewer, not the hub's data).
 */
import React, { useEffect, useMemo, useRef, useState } from 'react';
import { Box, Button, Dialog, DialogActions, DialogContent, DialogTitle, LinearProgress, Stack, TextField, Typography, IconButton } from '@mui/material';
import { useDsTokens } from '../../theme/useDsTokens';
import { useTelemetryEntities } from '../../hooks/useTelemetryEntities';
import { UnitsConfigJoin } from '../../hooks/useUnitsConfig';
import { mainUnit } from '../../pages/overviewData';
import { signedPowerW } from '../../entities/siteModel';
import { stepBetween, windowOf, loadVampires, saveVampires, LiveSample, StepResult, FoundVampire, MIN_SAMPLES, robustSd } from '../../economics/vampireHunt';
import { monthlyKwh } from '../../economics/vampires';
import { isDemo, getBaseUrl } from '../../services/connection';
import { nf, pwW, enWh } from '../common/ruFormat';
import { t } from '../../i18n';

const WINDOW_MS = 30_000;
const SETTLE_MS = 3_000;

type Stage = 'intro' | 'baseline' | 'ready' | 'after' | 'result';

export interface VampireHuntDialogProps {
  open: boolean;
  onClose: () => void;
  join: UnitsConfigJoin;
  /** Week-average buying price (a standby load runs round the clock); null → no money. */
  weekRate: number | null;
  fmt: (v: number | null | undefined) => string;
}

export const VampireHuntDialog: React.FC<VampireHuntDialogProps> = ({ open, onClose, join, weekRate, fmt }) => {
  const ds = useDsTokens();
  const { data } = useTelemetryEntities();
  const device = isDemo() ? 'demo' : getBaseUrl();
  const samples = useRef<LiveSample[]>([]);
  const [stage, setStage] = useState<Stage>('intro');
  const [t0, setT0] = useState(() => Date.now());
  const [mark, setMark] = useState(0);
  const [now, setNow] = useState(() => Date.now());
  const [result, setResult] = useState<StepResult | null>(null);
  const [name, setName] = useState('');
  const [found, setFound] = useState<FoundVampire[]>(() => loadVampires(device));

  const main = data ? mainUnit(data.units, join) : undefined;
  // Without the hub's resolution (p_res_w) a 0,2 W guess: Ф6(a) 17.10 on .139 read σ ≈ 0,09 W on a
  // 34 W base, and the old 0,5 W (floor 1 W) hid a measured +0,54 W step; noisy mains still raise
  // the floor through 3 × noise.
  const resolutionW = main?.pResW != null ? Math.max(0.05, main.pResW) : 0.2;

  // Every live frame of the input is a sample (the hub pushes about once a second).
  useEffect(() => {
    if (!open || !main?.present) return;
    const p = signedPowerW(main);
    if (p == null) return;
    const s: LiveSample = { t: Date.now(), p, ph: main.phases?.filter((r) => r.p != null).map((r) => ({ ph: r.ph, p: r.p as number })) };
    samples.current = [...samples.current.filter((x) => x.t > s.t - 5 * 60_000), s];
  }, [data, open, main]);

  useEffect(() => {
    if (!open) return undefined;
    const id = setInterval(() => setNow(Date.now()), 500);
    return () => clearInterval(id);
  }, [open]);

  const restart = () => { samples.current = []; setT0(Date.now()); setStage('baseline'); setResult(null); setName(''); };
  useEffect(() => { if (open) { restart(); setStage('intro'); } }, [open]); // eslint-disable-line react-hooks/exhaustive-deps

  const before = useMemo(() => windowOf(samples.current, (mark || now) - WINDOW_MS, mark || now), [mark, now]);
  const live = samples.current.length ? samples.current[samples.current.length - 1].p : null;
  const baseElapsed = now - t0;
  // Baseline is ready after 30 s with enough frames.
  useEffect(() => {
    if (stage === 'baseline' && baseElapsed >= WINDOW_MS && windowOf(samples.current, now - WINDOW_MS, now).length >= MIN_SAMPLES) setStage('ready');
  }, [stage, baseElapsed, now]);
  const afterElapsed = mark ? now - mark - SETTLE_MS : 0;
  useEffect(() => {
    if (stage !== 'after' || afterElapsed < WINDOW_MS) return;
    const r = stepBetween(before, windowOf(samples.current, mark + SETTLE_MS, mark + SETTLE_MS + WINDOW_MS), resolutionW);
    setResult(r);
    setStage('result');
  }, [stage, afterElapsed, before, mark, resolutionW]);

  const switchedOff = () => { setMark(Date.now()); setStage('after'); };
  // What the baseline says the meter can tell apart — shown BEFORE the user switches anything off
  // (3φ audit H3P-17: the 14 W threshold showed only after a failed try).
  const readyFloor = useMemo(() => {
    if (stage !== 'ready') return null;
    const b = windowOf(samples.current, now - WINDOW_MS, now).map((x) => x.p);
    if (b.length < MIN_SAMPLES) return null;
    return Math.max(2 * resolutionW, 3 * robustSd(b)); // the same noise rule as the verdict
  }, [stage, now, resolutionW]);
  const standbyW = result ? -result.deltaW : 0;
  const kwh = monthlyKwh(Math.max(0, standbyW));
  const save = () => {
    if (!result) return;
    const list = [...found, { name: name.trim() || t('hunt.unnamed', { n: found.length + 1 }), w: standbyW, phase: result.phase, at: Date.now() }];
    setFound(list);
    saveVampires(device, list);
    restart();
  };
  const remove = (i: number) => { const list = found.filter((_, k) => k !== i); setFound(list); saveVampires(device, list); };
  const totalW = found.reduce((s, v) => s + Math.max(0, v.w), 0);
  const money = (w: number) => (weekRate != null ? ` · ≈ ${fmt(monthlyKwh(w) * weekRate)}${t('units.perMonth')}` : '');
  const num = { fontVariantNumeric: 'tabular-nums' as const };

  const progress = stage === 'baseline' ? Math.min(100, (baseElapsed / WINDOW_MS) * 100) : stage === 'after' ? Math.max(0, Math.min(100, (afterElapsed / WINDOW_MS) * 100)) : null;

  return (
    <Dialog open={open} onClose={onClose} maxWidth="sm" fullWidth>
      <DialogTitle>{t('hunt.title')}</DialogTitle>
      <DialogContent>
        <Stack spacing={2}>
          <Typography sx={{ ...num, fontSize: 14, color: ds.inkMuted }}>
            {t('hunt.liveNow')}: <Box component="span" sx={{ color: live != null && live < 0 ? ds.flowExport : ds.ink, fontWeight: 600 }}>{live == null ? '—' : live < 0 ? `↑ ${pwW(-live)}` : pwW(live)}</Box>
            {main?.phases?.length ? ` · ${main.phases.map((r) => `L${r.ph} ${pwW(r.p)}`).join(' · ')}` : ''}
          </Typography>
          {!main?.present && <Typography sx={{ color: ds.sevWarning, fontSize: 14 }}>{t('hunt.noInput')}</Typography>}
          {/* Generation moves the input by tens of watts a second — a 4 W step drowns in it. */}
          {live != null && live < 0 && <Typography sx={{ color: ds.inkMuted, fontSize: 13 }}>{t('hunt.exportHint')}</Typography>}

          {stage === 'intro' && <Typography>{t('hunt.intro')}</Typography>}
          {stage === 'baseline' && <Typography>{t('hunt.stepBaseline')}</Typography>}
          {stage === 'ready' && <Typography>{t('hunt.stepSwitch')}</Typography>}
          {stage === 'ready' && readyFloor != null && (
            <Typography sx={{ ...num, fontSize: 13, color: ds.inkMuted }}>{t('hunt.floorNow', { floor: pwW(readyFloor) })}</Typography>
          )}
          {stage === 'after' && <Typography>{t('hunt.stepAfter')}</Typography>}
          {progress != null && <LinearProgress variant="determinate" value={progress} aria-label={t('hunt.measuring')} />}

          {stage === 'result' && (result == null ? (
            <Typography sx={{ color: ds.sevWarning }}>{t('hunt.fewSamples')}</Typography>
          ) : result.significant && standbyW > 0 ? (
            <Stack spacing={1}>
              <Typography sx={{ ...num, fontSize: 18, fontWeight: 700 }}>
                −{pwW(standbyW)}{result.phase ? ` (L${result.phase})` : ''}
              </Typography>
              <Typography sx={{ ...num, fontSize: 14, color: ds.inkMuted }}>
                {t('hunt.perMonth', { energy: enWh(kwh * 1000) })}{money(standbyW)}
              </Typography>
              <TextField size="small" label={t('hunt.nameLabel')} placeholder={t('hunt.namePlaceholder')} value={name} onChange={(e) => setName(e.target.value)} />
            </Stack>
          ) : (
            <Typography sx={{ ...num, color: ds.inkMuted }}>
              {standbyW <= 0 && result.significant
                ? t('hunt.rose', { power: pwW(result.deltaW) })
                : t('hunt.notSeen', { power: pwW(Math.abs(result.deltaW)), floor: pwW(result.floorW) })}
            </Typography>
          ))}

          {found.length > 0 && (
            <Box sx={{ pt: 1, borderTop: `1px solid ${ds.border}` }}>
              <Typography sx={{ fontWeight: 600, fontSize: 14, mb: 0.5 }}>
                {t('hunt.foundTitle', { n: found.length })} — {pwW(totalW)}{money(totalW)}
              </Typography>
              {found.map((v, i) => (
                <Stack key={`${v.at}-${i}`} direction="row" alignItems="center" justifyContent="space-between" sx={{ ...num, fontSize: 14 }}>
                  <Typography sx={{ fontSize: 14 }}>{v.name}{v.phase ? ` · L${v.phase}` : ''}</Typography>
                  <Stack direction="row" alignItems="center" spacing={1}>
                    <Typography sx={{ ...num, fontSize: 14, color: ds.inkMuted }}>{pwW(v.w)} · {nf(monthlyKwh(v.w), 1)} {t('units.kWh')}{t('units.perMonth')}</Typography>
                    <IconButton size="small" aria-label={t('hunt.remove', { name: v.name })} onClick={() => remove(i)}>×</IconButton>
                  </Stack>
                </Stack>
              ))}
              <Typography sx={{ fontSize: 12, color: ds.inkMuted, mt: 0.5 }}>{t('hunt.listNote')}</Typography>
            </Box>
          )}
        </Stack>
      </DialogContent>
      <DialogActions>
        {stage === 'intro' && <Button variant="contained" disabled={!main?.present} onClick={restart}>{t('hunt.begin')}</Button>}
        {stage === 'ready' && <Button variant="contained" onClick={switchedOff}>{t('hunt.switchedOff')}</Button>}
        {stage === 'result' && result?.significant && standbyW > 0 && <Button variant="contained" onClick={save}>{t('hunt.save')}</Button>}
        {stage === 'result' && <Button onClick={restart}>{t('hunt.again')}</Button>}
        <Button onClick={onClose}>{t('common.close')}</Button>
      </DialogActions>
    </Dialog>
  );
};
