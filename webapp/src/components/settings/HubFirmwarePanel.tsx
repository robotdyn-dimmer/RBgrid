/**
 * HubFirmwarePanel — update the HUB's own firmware (POST /api/system/ota, raw .bin). The image is
 * read before sending (hubFw.ts): project / chip must match the hub, version and build date are
 * shown next to what the hub runs, SHA-256 goes along where the browser can hash. Then:
 * upload (progress) → the hub restarts by itself → back on the network → the new image confirms
 * itself 60 s after Wi-Fi is up, or the bootloader rolls back (read from the hub log).
 */
import React, { useEffect, useRef, useState } from 'react';
import { Alert, Box, Button, LinearProgress, Stack, Typography } from '@mui/material';
import SystemUpdateAltIcon from '@mui/icons-material/SystemUpdateAlt';
import { PanelCard } from '@rbgrid/design-system';
import { useCapabilities, useDeviceSession } from '../../core';
import { getApiClient } from '../../services/api';
import { isDemo } from '../../services/connection';
import type { HubVersion } from '../../types/hubEvents';
import { t } from '../../i18n';
import { useConfirm } from '../common/ConfirmDialog';
import { sha256Hex } from '../devices/moduleFw';
import { AppImage, parseAppImage, imageProblem, sameBuild, otaVerdict } from './hubFw';

type Stage = 'idle' | 'uploading' | 'restarting' | 'confirming' | 'confirmed' | 'rolledBack' | 'unconfirmed' | 'error';
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

export const HubFirmwarePanel: React.FC = () => {
  const session = useDeviceSession();
  const caps = useCapabilities();
  const confirm = useConfirm();
  const input = useRef<HTMLInputElement>(null);
  const alive = useRef(true);
  const [hub, setHub] = useState<HubVersion | null>(null);
  const [file, setFile] = useState<{ name: string; buf: ArrayBuffer; img: AppImage | null; problem: string | null } | null>(null);
  const [stage, setStage] = useState<Stage>('idle');
  const [pct, setPct] = useState(0);
  const [err, setErr] = useState('');

  const api = getApiClient();
  useEffect(() => {
    alive.current = true;
    api.getHubVersion().then(setHub).catch(() => setHub(null));
    return () => { alive.current = false; };
  }, [api]);

  const pick = async (f: File | undefined) => {
    setErr(''); setStage('idle');
    if (!f) return;
    const buf = await f.arrayBuffer();
    const parsed = parseAppImage(buf);
    if (typeof parsed === 'string') { setFile({ name: f.name, buf, img: null, problem: parsed }); return; }
    setFile({ name: f.name, buf, img: parsed, problem: imageProblem(parsed, { project: hub?.project, hwProfile: caps.hwProfile }) });
  };

  const run = async () => {
    if (!file?.img) return;
    if (!(await confirm({ title: t('hubFw.confirmTitle'), body: t('hubFw.confirmBody'), confirmLabel: t('hubFw.install') }))) return;
    const before = hub;
    setErr(''); setPct(0); setStage('uploading');
    try {
      const sha256 = await sha256Hex(file.buf);
      await session.transport.postOta(file.buf, { ...(sha256 ? { sha256 } : {}), onProgress: (p) => alive.current && setPct(Math.round(p * 100)) });
    } catch (e) {
      const status = (e as { response?: { status?: number; data?: { message?: string; error_code?: string } } })?.response;
      // fw 1.1.6: a second image within ~60 s of the last one → 409 IMAGE_NOT_CONFIRMED (was a bare 500).
      setErr(status?.status === 401 || status?.status === 403 ? t('common.adminTokenRequired')
        : status?.data?.error_code === 'IMAGE_NOT_CONFIRMED' ? t('hubFw.notConfirmedYet')
          : String(status?.data?.message ?? (e as Error)?.message ?? e));
      setStage('error');
      return;
    }
    // The hub restarts ~1 s after its answer: wait for it to drop, then to come back.
    setStage('restarting');
    const fast = isDemo();
    let back: HubVersion | null = null;
    const t0 = Date.now();
    let wentDown = fast;
    while (Date.now() - t0 < 180_000 && alive.current) {
      await sleep(fast ? 500 : 3000);
      try {
        const v = await api.getHubVersion({ timeout: 2500 } as never);
        if (wentDown || v.image_md5 !== before?.image_md5 || Date.now() - t0 > 25_000) { back = v; break; }
      } catch { wentDown = true; }
    }
    if (!back) { setErr(t('hubFw.notBack')); setStage('error'); return; }
    setHub(back);
    // The new image confirms itself 60 s after Wi-Fi is up; a reset before that rolls it back.
    setStage('confirming');
    const c0 = Date.now();
    while (Date.now() - c0 < (fast ? 3000 : 150_000) && alive.current) {
      await sleep(fast ? 1000 : 10_000);
      if (fast) { setStage('confirmed'); return; }
      try {
        const page = await api.getHubEventsPage({ level: 'all' });
        const v = otaVerdict(page.events.map((e) => e.msg));
        if (v) { setStage(v); api.getHubVersion().then(setHub).catch(() => undefined); return; }
      } catch { /* hub busy — next round */ }
    }
    setStage('unconfirmed');
  };

  // A hub whose schema doesn't list the OTA endpoint can't take it (older / OSS builds).
  if (caps.fromSchema && !caps.endpoints.some((e) => e.path === '/api/system/ota')) return null;
  const running = stage === 'uploading' || stage === 'restarting' || stage === 'confirming';
  const img = file?.img;
  return (
    <PanelCard title={t('hubFw.title')}>
      <Stack spacing={1.5}>
        <Typography variant="body2" color="text.secondary">{t('hubFw.intro')}</Typography>
        {hub && (
          <Typography variant="body2">
            {t('hubFw.current', { version: hub.version ?? '—', built: hub.build_date ?? '—', image: (hub.image_md5 ?? '').slice(0, 8) || '—' })}
          </Typography>
        )}
        <input ref={input} type="file" accept=".bin" hidden onChange={(e) => { void pick(e.target.files?.[0]); e.target.value = ''; }} />
        <Box>
          <Button variant="outlined" startIcon={<SystemUpdateAltIcon />} disabled={running} onClick={() => input.current?.click()}>
            {file ? t('hubFw.otherFile') : t('hubFw.pick')}
          </Button>
        </Box>
        {file && (
          <Typography variant="body2" sx={{ wordBreak: 'break-all' }}>
            {file.name} · {Math.round(file.buf.byteLength / 1024)} KB
            {img ? ` · ${t('hubFw.image', { version: img.version, built: img.built, idf: img.idf })}` : ''}
          </Typography>
        )}
        {file?.problem && <Alert severity="error">{t(`hubFw.problem_${file.problem}`, { project: hub?.project ?? '—' })}</Alert>}
        {img && !file?.problem && hub && sameBuild(img, hub) && <Alert severity="info">{t('hubFw.sameBuild')}</Alert>}
        {img && !file?.problem && stage === 'idle' && (
          <Box><Button variant="contained" onClick={run}>{t('hubFw.install')}</Button></Box>
        )}
        {running && (
          <Box>
            <Typography variant="body2" sx={{ mb: 0.5 }}>{t(`hubFw.stage_${stage}`, { pct })}</Typography>
            <LinearProgress variant={stage === 'uploading' ? 'determinate' : 'indeterminate'} value={pct} />
          </Box>
        )}
        {stage === 'confirmed' && <Alert severity="success">{t('hubFw.done', { version: hub?.version ?? '—', built: hub?.build_date ?? '—' })}</Alert>}
        {stage === 'rolledBack' && <Alert severity="error">{t('hubFw.rolledBack')}</Alert>}
        {stage === 'unconfirmed' && <Alert severity="warning">{t('hubFw.unconfirmed')}</Alert>}
        {stage === 'error' && <Alert severity="error">{t('hubFw.failed', { reason: err })}</Alert>}
      </Stack>
    </PanelCard>
  );
};
