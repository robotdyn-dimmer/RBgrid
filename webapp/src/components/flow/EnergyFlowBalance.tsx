/**
 * EnergyFlowBalance — the Аналитика «Баланс» schema (spec §4.4 / §5), preferred on narrow
 * screens. Pure SVG drawn in real pixels for its container width (labels stay 11–12 px at
 * every width); rebuilt only when a rounded value signature or the width changes (no flicker).
 */
import React, { useLayoutEffect, useMemo, useRef } from 'react';
import { Box } from '@mui/material';
import { morphSvg } from './morphSvg';
import { useDsTokens } from '../../theme/useDsTokens';
import { FlowFrame, flowIdle } from './flowFrame';
import { FlowIdle } from './FlowIdle';
import { flowCssVars } from './EnergyFlowSankey';
import { balanceSkeleton, balanceSignature, BALANCE_MIN_W } from './flowBalance';
import { useElementWidth } from '../../hooks/useElementWidth';

/** Widest the balance is drawn; wider cards centre it. */
const BALANCE_MAX_W = 960;

export interface EnergyFlowBalanceProps {
  frame: FlowFrame;
}

export const EnergyFlowBalance: React.FC<EnergyFlowBalanceProps> = ({ frame }) => {
  const ds = useDsTokens();
  const hostRef = useRef<HTMLDivElement>(null);
  const [measureRef, width] = useElementWidth<HTMLDivElement>(760);
  const W = Math.max(BALANCE_MIN_W, Math.min(BALANCE_MAX_W, width));
  const sig = balanceSignature(frame);
  // eslint-disable-next-line react-hooks/exhaustive-deps
  const svg = useMemo(() => balanceSkeleton(frame, W), [sig, W]);
  const idle = flowIdle(frame);
  useLayoutEffect(() => {
    // Values morph into the mounted SVG (same element tree) — no recreation (§10.2).
    if (hostRef.current) morphSvg(hostRef.current, svg);
  }, [svg, idle]);

  if (idle) return <FlowIdle />;
  return (
    // Below BALANCE_MIN_W (never in practice: 320 phone − gutters ≈ 288) it scrolls instead of shrinking.
    // `contain: inline-size` — the pixel-wide svg must not feed back into the container's
    // intrinsic width (else a flex/grid parent could never shrink below the last W).
    <Box ref={measureRef} sx={{ width: '100%', minWidth: 0, overflowX: 'auto', contain: 'inline-size' }}>
      <Box
        ref={hostRef}
        sx={{ ...flowCssVars(ds), width: W, mx: 'auto', opacity: frame.lost ? 0.5 : 1, '& svg': { display: 'block' } }}
        aria-hidden={false}
      />
    </Box>
  );
};
