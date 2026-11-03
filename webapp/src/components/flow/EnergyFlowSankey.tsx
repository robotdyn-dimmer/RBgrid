/**
 * EnergyFlowSankey — the Аналитика «Распределение» schema (spec §4.4 / §5). Pure SVG, no
 * chart.js. The renderer is a continuous function of the values, so the SVG is rebuilt only
 * when a rounded value/structure signature changes (no flicker, no animation to restart).
 *
 * Narrow containers (< {@link SANKEY_MIN_W} px, i.e. below 0.9× the native 760) render
 * EnergyFlowBalance instead (spec §5: Balance for narrow screens; review 03 §5.2 scale rule).
 */
import React, { useLayoutEffect, useMemo, useRef } from 'react';
import { Box } from '@mui/material';
import { useDsTokens } from '../../theme/useDsTokens';
import { dsFonts } from '../../theme/dsTokens';
import type { DsPalette } from '../../theme/dsTokens';
import { FlowFrame, flowIdle } from './flowFrame';
import { FlowIdle } from './FlowIdle';
import { sankeySkeleton, sankeySignature } from './flowSankey';
import { morphSvg } from './morphSvg';
import { useElementWidth } from '../../hooks/useElementWidth';
import { EnergyFlowBalance } from './EnergyFlowBalance';

/** Below this container width the Sankey would scale its text under 0.9× → Balance instead. */
export const SANKEY_MIN_W = 690;

/** Set every CSS var the flow renderers read (spec palette). */
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
    '--chart-grid': ds.chartGrid,
    '--bg': ds.bg,
  };
  ds.series.forEach((c, i) => { vars['--series-' + (i + 1)] = c; });
  return vars as React.CSSProperties;
}

export interface EnergyFlowSankeyProps {
  frame: FlowFrame;
}

export const EnergyFlowSankey: React.FC<EnergyFlowSankeyProps> = ({ frame }) => {
  const ds = useDsTokens();
  const hostRef = useRef<HTMLDivElement>(null);
  const [measureRef, width] = useElementWidth<HTMLDivElement>(760);
  const narrow = width < SANKEY_MIN_W;
  const sig = sankeySignature(frame);
  // eslint-disable-next-line react-hooks/exhaustive-deps
  const svg = useMemo(() => sankeySkeleton(frame), [sig]);
  const idle = flowIdle(frame);
  useLayoutEffect(() => {
    // Values morph into the mounted SVG (same element tree) — no recreation (§10.2).
    if (!narrow && hostRef.current) morphSvg(hostRef.current, svg);
  }, [svg, narrow, idle]);

  if (idle) return <FlowIdle />;
  return (
    <Box ref={measureRef} sx={{ width: '100%', minWidth: 0 }}>
      {narrow ? (
        <EnergyFlowBalance frame={frame} />
      ) : (
        <Box
          ref={hostRef}
          sx={{ ...flowCssVars(ds), width: '100%', maxWidth: 760, mx: 'auto', opacity: frame.lost ? 0.5 : 1, '& svg': { display: 'block' } }}
          aria-hidden={false}
        />
      )}
    </Box>
  );
};
