/**
 * EnergyFlowHome — the house flow schema for the Home overview (spec §4.1 / §5).
 *
 * No-flicker contract (spec §2.3 / §14 W2): the SVG skeleton is built once per STRUCTURAL
 * key and injected imperatively; live values are patched in place (text/stroke-width/ring)
 * without recreating the DOM, so the SMIL flow dots keep animating on the compositor.
 * React never owns the SVG children (empty JSX), so re-renders don't clobber the patch.
 *
 * Single-device cases (spec §14 W3): with no measured lines the big schema explains nothing —
 * show the compact «Солнце → Дом ← Сеть» row instead, or nothing when there is no generation.
 *
 * Layout by the schema's OWN container width (review 03 P0-4/§5.2 — SVG text may only scale
 * within 0.9–1.25; spec §4.1 phone layout): below {@link HOME_COMPACT_BELOW} px an HTML
 * layout (compact «Солнце → Дом ← Сеть» row + one row per load), at/above it the SVG
 * (760 px native, capped at 760). 690 px = 760 × 0.9.
 *
 * Eco (spec §10.5, review 11): `prefers-reduced-motion: reduce` → skeleton without any
 * `<animateMotion>`; the SMIL timeline is paused while the tab is hidden or off-screen.
 */
import React, { useLayoutEffect, useMemo, useRef } from 'react';
import { Box, Stack, Typography } from '@mui/material';
import { useDsTokens } from '../../theme/useDsTokens';
import { dsFonts } from '../../theme/dsTokens';
import { FlowFrame } from './flowFrame';
import { homeStructuralKey, homeSkeleton, homePatch, pw, ICON, hysteresisBuckets, houseText } from './flowHome';
import { FlowLoad } from './flowFrame';
import { useElementWidth } from '../../hooks/useElementWidth';
import { usePrefersReducedMotion, useSmilEco } from './useFlowMotion';
import type { DsPalette } from '../../theme/dsTokens';
import { t } from '../../i18n';

export function flowCssVars(ds: DsPalette): React.CSSProperties {
  const vars: Record<string, string> = {
    '--font-sans': dsFonts.sans,
    '--font-mono': dsFonts.mono,
    '--ink': ds.ink,
    '--ink-muted': ds.inkMuted,
    '--surface': ds.surface,
    '--surface-raised': ds.surfaceRaised,
    '--flow-export': ds.flowExport,
    '--flow-import': ds.flowImport,
    '--solar': ds.solar,
    '--solar-ink': ds.solarInk,
    '--primary': ds.primary,
    '--border': ds.border,
    '--border-strong': ds.borderStrong,
    '--status-offline': ds.statusOffline,
    '--sev-alarm': ds.sevAlarm,
    '--bg': ds.bg,
  };
  return vars as React.CSSProperties;
}

/** Container width (px) below which the home schema switches to the HTML compact layout. */
export const HOME_COMPACT_BELOW = 690;

export interface EnergyFlowHomeProps {
  frame: FlowFrame;
  /** A group card (frame «Все» with G5 groups) was clicked → open that group's tab. */
  onOpenGroup?: (groupId: string) => void;
}

/**
 * Compact «Солнце → Дом ← Сеть». Солнце is omitted entirely without generation (spec §10.3:
 * no solar mentions when there is none) → «Дом ← Сеть».
 */
export const CompactFlow: React.FC<{ frame: FlowFrame; ds: DsPalette; dense?: boolean }> = ({ frame, ds, dense }) => {
  const exp = frame.grid < 0 && frame.gridHasDirection;
  const hasSolar = frame.solar != null;
  const aria = [
    ...(hasSolar ? [t('flow.ariaSolarCap', { power: pw(frame.solar) })] : []),
    t('flow.ariaHouse', { power: pw(frame.house) }),
    t(exp ? 'flow.ariaExport' : 'flow.ariaGrid', { power: pw(Math.abs(frame.grid)) }),
  ].join(', ');
  const arrow = { color: ds.inkMuted, fontSize: dense ? 16 : undefined };
  return (
    <Stack direction="row" spacing={dense ? 1.5 : 3} alignItems="center" justifyContent="center" sx={{ py: dense ? 1 : 2 }}
      role="img" aria-label={aria}>
      {hasSolar && <Figure label={t('flow.solar')} value={pw(frame.solar)} color={ds.solar} dense={dense} />}
      {hasSolar && <Typography sx={arrow}>→</Typography>}
      <Figure label={t('flow.home')} value={houseText(frame)} color={ds.ink} dense={dense} />
      <Typography sx={arrow}>{exp ? '→' : '←'}</Typography>
      <Figure label={t(exp ? 'flow.exporting' : 'flow.grid')} value={frame.gridPresent === false ? '—' : pw(Math.abs(frame.grid))} color={exp ? ds.flowExport : ds.flowImport} dense={dense} />
    </Stack>
  );
};

const Figure: React.FC<{ label: string; value: string; color: string; dense?: boolean }> = ({ label, value, color, dense }) => (
  <Stack alignItems="center" spacing={0.25} sx={{ minWidth: 0 }}>
    <Typography sx={{ fontSize: 12, letterSpacing: '0.02em' }} color="text.secondary">{label}</Typography>
    <Typography sx={{ fontSize: dense ? 18 : 22, fontWeight: 600, color, whiteSpace: 'nowrap', fontVariantNumeric: 'tabular-nums' }}>{value}</Typography>
  </Stack>
);

/** One load as a plain row: icon · name · value («нет данных» for null). A group row opens its tab. */
const LoadRow: React.FC<{ load: FlowLoad; ds: DsPalette; onOpenGroup?: (id: string) => void }> = ({ load: l, ds, onOpenGroup }) => {
  const iconColor = l.v == null ? ds.statusOffline : l.alarm ? ds.sevAlarm : l.group ? ds.primary : ds.inkMuted;
  const valColor = l.alarm ? ds.sevAlarm : l.v == null ? ds.inkMuted : ds.ink;
  const open = l.group && onOpenGroup ? () => onOpenGroup((l.group as { id: string }).id) : undefined;
  return (
    <Stack direction="row" alignItems="center" spacing={1.5}
      component={open ? 'button' : 'div'} onClick={open} type={open ? 'button' : undefined}
      sx={{ py: 1, px: 1.5, borderRadius: 1.5, border: `1px solid ${l.alarm ? ds.sevAlarm : l.group ? ds.borderStrong : ds.border}`, bgcolor: ds.surfaceRaised, minWidth: 0,
        width: '100%', font: 'inherit', textAlign: 'left', cursor: open ? 'pointer' : undefined }}>
      <Box component="svg" viewBox="0 0 24 24" sx={{ width: 20, height: 20, flexShrink: 0 }} aria-hidden>
        <path d={ICON[l.icon] ?? ICON.plug} fill="none" stroke={iconColor} strokeWidth={1.75} strokeLinecap="round" strokeLinejoin="round" />
      </Box>
      <Typography title={l.name} sx={{ flex: 1, minWidth: 0, fontSize: 14, color: ds.ink, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
        {l.group ? t('flow.groupCard', { name: l.name, count: l.group.count }) : l.name}
      </Typography>
      <Typography sx={{ flexShrink: 0, fontSize: 14, fontWeight: l.v == null ? 400 : 600, color: valColor, fontVariantNumeric: 'tabular-nums', whiteSpace: 'nowrap' }}>
        {pw(l.v)}{l.alarm ? ' ⚠' : ''}{open ? ' ›' : ''}
      </Typography>
    </Stack>
  );
};

/** Phone / narrow-card layout (spec §4.1): compact row on top, the loads as rows below. */
const CompactHome: React.FC<{ frame: FlowFrame; ds: DsPalette; onOpenGroup?: (id: string) => void }> = ({ frame, ds, onOpenGroup }) => (
  <Stack spacing={1} sx={{ opacity: frame.lost ? 0.5 : 1 }}>
    {frame.scope ? (
      <Stack direction="row" spacing={3} alignItems="center" justifyContent="center" sx={{ py: 1 }} role="img"
        aria-label={t('flow.ariaGroup', { name: frame.scope.name, power: pw(frame.house), count: frame.loads.length })}>
        <Figure label={t('flow.home')} value={pw(frame.scope.house)} color={ds.inkMuted} dense />
        <Typography sx={{ color: ds.inkMuted, fontSize: 16 }}>→</Typography>
        <Figure label={frame.scope.name} value={pw(frame.house)} color={ds.ink} dense />
      </Stack>
    ) : <CompactFlow frame={frame} ds={ds} dense />}
    {frame.loads.length > 0 && (
      <Stack spacing={0.75} component="ul" sx={{ listStyle: 'none', m: 0, p: 0 }} aria-label={t('flow.loadsAria')}>
        {frame.loads.map((l) => (
          <Box component="li" key={l.id}><LoadRow load={l} ds={ds} onOpenGroup={onOpenGroup} /></Box>
        ))}
      </Stack>
    )}
  </Stack>
);

export const EnergyFlowHome: React.FC<EnergyFlowHomeProps> = ({ frame, onOpenGroup }) => {
  const ds = useDsTokens();
  const hostRef = useRef<HTMLDivElement>(null);
  const frameRef = useRef(frame);
  frameRef.current = frame;
  // jsdom/SSR: no ResizeObserver → fallback 760 → the SVG layout (deterministic tests).
  const [measureRef, width] = useElementWidth<HTMLDivElement>(760);
  const reduceMotion = usePrefersReducedMotion();

  const hasLoads = frame.loads.length > 0;
  const wide = width >= HOME_COMPACT_BELOW;
  const showSvg = hasLoads && wide;

  const bucketsRef = useRef(hysteresisBuckets());
  const structuralKey = homeStructuralKey(frame, bucketsRef.current);
  // Rebuild the skeleton ONLY when the structure (or the motion preference) changes.
  const skeleton = useMemo(
    () => homeSkeleton(frameRef.current, { motion: !reduceMotion }),
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [structuralKey, reduceMotion],
  );

  // Inject the skeleton imperatively whenever it changes or the SVG host (re)mounts.
  useLayoutEffect(() => {
    if (!showSvg || !hostRef.current) return;
    hostRef.current.innerHTML = skeleton;
    // Patch immediately so the first paint shows current values.
    const svg = hostRef.current.querySelector('svg');
    if (svg) homePatch(svg, frameRef.current);
  }, [skeleton, showSvg]);

  // Patch live values on every frame (same structure → no rebuild, dots keep running).
  useLayoutEffect(() => {
    if (!showSvg) return;
    const svg = hostRef.current?.querySelector('svg');
    if (svg) homePatch(svg, frame);
  }, [frame, showSvg]);

  // Eco: pause SMIL while hidden / off-screen (re-attaches on rebuild or layout switch).
  useSmilEco(hostRef, showSvg ? skeleton : null);

  if (!hasLoads) {
    if (frame.solar == null) return null; // single-device, no generation → PowerHero alone
    return <CompactFlow frame={frame} ds={ds} />;
  }

  return (
    <Box ref={measureRef} sx={{ width: '100%', minWidth: 0 }}>
      {showSvg ? (
        // Native 760 wide (height grows with the loads); capped + centred so it doesn't balloon on
        // wide cards. Above 690 px the effective SVG scale stays within 0.9–1.0 (no text < 11 px).
        // Group cards are SVG <g data-group>; a click opens the tab (keyboard: the tabs themselves).
        <Box
          ref={hostRef}
          onClick={onOpenGroup ? (e: React.MouseEvent) => {
            const g = (e.target as Element).closest?.('[data-group]');
            const id = g?.getAttribute('data-group');
            if (id) onOpenGroup(id);
          } : undefined}
          sx={{ ...flowCssVars(ds), width: '100%', maxWidth: 760, mx: 'auto', '& svg': { display: 'block' } }}
          aria-hidden={false}
        />
      ) : (
        <CompactHome frame={frame} ds={ds} onOpenGroup={onOpenGroup} />
      )}
    </Box>
  );
};
