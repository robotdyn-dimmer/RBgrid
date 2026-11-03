/**
 * EnergySceneHome — isometric «Сцена» of the Home overview (review 11 §7; option next to the
 * flat «Схема», default stays «Схема»). Loaded lazily (React.lazy) only when chosen.
 *
 * - SVG built by flowScene once per structural key and injected imperatively (no-flicker rule
 *   §2.3: values never recreate the scene); SMIL dots only without prefers-reduced-motion and
 *   paused while hidden/off-screen (useSmilEco).
 * - Numbers are a FLAT HTML layer at anchors from the same projection: never skewed, never
 *   below 12/14 px, updated by React in place.
 * - Container < 560 px: labels collapse to numbered keys, values move to a list under the scene
 *   (phones keep the illustration + every value); < 280 px: the compact «Солнце → Дом ← Сеть» row.
 */
import React, { useLayoutEffect, useMemo, useRef } from 'react';
import { Box, Stack, Typography } from '@mui/material';
import { useDsTokens } from '../../theme/useDsTokens';
import { useTheme } from '../../theme/ThemeProvider';
import { useElementWidth } from '../../hooks/useElementWidth';
import { FlowFrame } from './flowFrame';
import { buildScene, sceneStructuralKey, SCENE_W, SCENE_H, SceneAnchor } from './flowScene';
import { pw, hysteresisBuckets, gridValText, houseText } from './flowHome';
import { CompactFlow, flowCssVars } from './EnergyFlowHome';
import { usePrefersReducedMotion, useSmilEco } from './useFlowMotion';
import { ISO_TOKENS } from './iso';
import { t } from '../../i18n';

const KEYS_BELOW = 560;
const COMPACT_BELOW = 280; // phones keep the scene (numbered keys + value list); only tiny cards fall back

interface Label { id: string; name: string; value: string; alarm?: boolean; color?: string }

const EnergySceneHome: React.FC<{ frame: FlowFrame }> = ({ frame }) => {
  const ds = useDsTokens();
  const { isDarkMode } = useTheme();
  const reduceMotion = usePrefersReducedMotion();
  const [measureRef, width] = useElementWidth<HTMLDivElement>(880);
  const hostRef = useRef<HTMLDivElement>(null);
  const frameRef = useRef(frame);
  frameRef.current = frame;

  const exp = frame.grid < 0 && frame.gridHasDirection;
  const hasSolar = frame.solar != null;
  const aria = [
    ...(hasSolar ? [t('flow.ariaSolarCap', { power: pw(frame.solar) })] : []),
    t('flow.ariaHouse', { power: pw(frame.house) }),
    t(exp ? 'flow.ariaExport' : 'flow.ariaGrid', { power: pw(Math.abs(frame.grid)) }),
  ].join(', ');

  const bucketsRef = useRef(hysteresisBuckets());
  const key = sceneStructuralKey(frame, bucketsRef.current);
  const scene = useMemo(
    () => buildScene(frameRef.current, { motion: !reduceMotion, aria: t('scene.aria', { summary: aria }) }),
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [key, reduceMotion],
  );
  const compact = width < COMPACT_BELOW;
  useLayoutEffect(() => {
    if (!compact && hostRef.current) hostRef.current.innerHTML = scene.svg;
  }, [scene, compact]);
  useSmilEco(hostRef, compact ? null : scene);

  if (compact) return <Box ref={measureRef}><CompactFlow frame={frame} ds={ds} /></Box>;

  const share = hasSolar && frame.house > 0 ? Math.round(Math.min(1, (frame.solar ?? 0) / frame.house) * 100) : null;
  const labels: Label[] = [
    { id: 'grid', name: t(exp ? 'scene.gridExport' : 'scene.gridImport'), value: gridValText(frame),
      color: exp ? ds.flowExport : undefined },
    ...(hasSolar ? [{ id: 'solar', name: t('flow.solar'), value: pw(frame.solar) }] : []),
    { id: 'house', name: share != null ? t('scene.houseShare', { pct: share }) : t('flow.home'), value: houseText(frame) },
    ...scene.loads.map((l) => ({ id: l.id, name: l.name, value: pw(l.v) + (l.alarm ? ' ⚠' : ''), alarm: l.alarm })),
  ];
  const byId = new Map(scene.anchors.map((a) => [a.id, a]));
  const keys = width < KEYS_BELOW;
  const iso = isDarkMode ? ISO_TOKENS.dark : ISO_TOKENS.light;
  const vars = {
    ...flowCssVars(ds),
    '--iso-top': iso.top, '--iso-left': iso.left, '--iso-right': iso.right, '--iso-edge': iso.edge,
    '--iso-ground': iso.ground, '--iso-ground-line': iso.groundLine,
  } as React.CSSProperties;

  const chip = (lb: Label, i: number, a: SceneAnchor) => (
    <Box
      key={lb.id}
      sx={{
        position: 'absolute',
        left: `calc(${((a.x / SCENE_W) * 100).toFixed(2)}% + ${keys ? 0 : a.dx}px)`,
        top: `${((a.y / SCENE_H) * 100).toFixed(2)}%`,
        transform: keys ? 'translate(-50%,-50%)' : a.left ? 'translate(-100%,-50%)' : 'translate(0,-50%)',
        ...(keys
          ? { width: 22, height: 22, borderRadius: '50%', bgcolor: ds.surfaceRaised, border: `1px solid ${lb.alarm ? ds.sevAlarm : ds.borderStrong}`,
            fontSize: 12, fontWeight: 600, lineHeight: '20px', textAlign: 'center', color: lb.alarm ? ds.sevAlarm : ds.ink }
          : { bgcolor: ds.surface, border: `1px solid ${lb.alarm ? ds.sevAlarm : ds.border}`, borderRadius: 1, px: 1, py: 0.5,
            whiteSpace: 'nowrap', boxShadow: '0 2px 8px rgba(0,0,0,.25)' }),
      }}
    >
      {keys ? i + 1 : (
        <>
          <Typography component="span" sx={{ display: 'block', fontSize: 12, lineHeight: '16px', color: ds.inkMuted }}>{lb.name}</Typography>
          <Typography component="span" sx={{ display: 'block', fontSize: 14, lineHeight: '20px', fontWeight: 600, fontVariantNumeric: 'tabular-nums',
            color: lb.alarm ? ds.sevAlarm : lb.color ?? ds.ink }}>{lb.value}</Typography>
        </>
      )}
    </Box>
  );

  return (
    <Box ref={measureRef} sx={{ ...vars, width: '100%', maxWidth: 960, mx: 'auto' }}>
      <Box sx={{ position: 'relative', '& svg': { display: 'block', width: '100%', height: 'auto' } }}>
        <Box ref={hostRef} />
        {labels.map((lb, i) => { const a = byId.get(lb.id); return a ? chip(lb, i, a) : null; })}
      </Box>
      {keys && (
        <Stack component="ul" sx={{ listStyle: 'none', m: 0, mt: 1.5, p: 0 }} aria-label={t('scene.legendAria')}>
          {labels.map((lb, i) => (
            <Stack component="li" key={lb.id} direction="row" alignItems="center" spacing={1}
              sx={{ py: 1, borderTop: `1px solid ${ds.border}`, fontSize: 14 }}>
              <Box sx={{ width: 22, height: 22, flexShrink: 0, borderRadius: '50%', bgcolor: ds.surfaceRaised, border: `1px solid ${ds.borderStrong}`,
                fontSize: 12, fontWeight: 600, lineHeight: '20px', textAlign: 'center', color: ds.ink }}>{i + 1}</Box>
              <Typography sx={{ fontSize: 14, color: ds.ink, minWidth: 0, flex: 1, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{lb.name}</Typography>
              <Typography sx={{ fontSize: 14, fontWeight: 600, fontVariantNumeric: 'tabular-nums', color: lb.alarm ? ds.sevAlarm : lb.color ?? ds.ink }}>{lb.value}</Typography>
            </Stack>
          ))}
        </Stack>
      )}
      <Typography sx={{ mt: 1, fontSize: 12, color: ds.inkMuted }}>{t('scene.note')}</Typography>
    </Box>
  );
};

export default EnergySceneHome;
