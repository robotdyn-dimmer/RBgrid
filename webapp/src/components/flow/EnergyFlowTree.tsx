/**
 * EnergyFlowTree — the single-line diagram for Предприятие and Станции (spec §4.2/§4.3/§5).
 * Pure SVG; the skeleton is rebuilt only when the STRUCTURE changes, values are patched in
 * place (spec §10.2 — no recreation, no flicker).
 *
 * Narrow cards scroll instead of shrinking (review 03 P0-5): the SVG keeps its native 760 px
 * (scale 1 → no text below 11 px, wave-4 check) inside a keyboard-scrollable region.
 */
import React, { useLayoutEffect, useMemo, useRef } from 'react';
import { Box } from '@mui/material';
import { useDsTokens } from '../../theme/useDsTokens';
import { flowCssVars } from './EnergyFlowSankey';
import { TreeData, treeSkeleton, treeSignature, treePatch } from './flowTree';
import { FlowLegend } from './FlowLegend';
import { t } from '../../i18n';

/** Scroll (don't shrink) below this width. */
export const TREE_MIN_W = 760;

export interface EnergyFlowTreeProps {
  data: TreeData;
}

export const EnergyFlowTree: React.FC<EnergyFlowTreeProps> = ({ data }) => {
  const ds = useDsTokens();
  const hostRef = useRef<HTMLDivElement>(null);
  const dataRef = useRef(data);
  dataRef.current = data;
  const sig = treeSignature(data);
  // eslint-disable-next-line react-hooks/exhaustive-deps
  const svg = useMemo(() => treeSkeleton(dataRef.current), [sig]);
  useLayoutEffect(() => {
    if (!hostRef.current) return;
    hostRef.current.innerHTML = svg;
    const el = hostRef.current.querySelector('svg');
    if (el) treePatch(el, dataRef.current);
  }, [svg]);
  // Same structure → patch the values in place (no recreation).
  useLayoutEffect(() => {
    const el = hostRef.current?.querySelector('svg');
    if (el) treePatch(el, data);
  }, [data]);

  return (
    <>
    <Box sx={{ width: '100%', minWidth: 0, overflowX: 'auto', WebkitOverflowScrolling: 'touch' }}
      tabIndex={0} role="region" aria-label={t('flow.treeRegion')}>
      <Box
        ref={hostRef}
        sx={{ ...flowCssVars(ds), width: '100%', minWidth: TREE_MIN_W, maxWidth: 760, mx: 'auto', opacity: data.lost ? 0.5 : 1, '& svg': { display: 'block' } }}
        aria-hidden={false}
      />
    </Box>
    <FlowLegend
      exportable={data.gridHasDirection && ((data.sources ?? []).length > 0 || data.grid < 0)}
      solar={(data.sources ?? []).length > 0}
      noData={data.groups.some((g) => g.kids.some((k) => k.v == null)) || (data.sources ?? []).some((k) => k.v == null)}
    />
    </>
  );
};
