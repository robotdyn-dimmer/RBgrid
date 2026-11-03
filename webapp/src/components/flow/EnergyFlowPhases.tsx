/**
 * EnergyFlowPhases — the flow card's «По фазам» view for a multi-phase input (operator 16.10: the
 * card showed only the total on a 3φ site). Grid → L1 / L2 / L3 (the input's measured `ph[]`) →
 * under each phase its single-phase lines and the rest not metered on that phase; lines that span
 * phases (3-phase, two-pole) sit in one row below. HTML, not SVG: it wraps to a column on phones.
 */
import React from 'react';
import { Box, Stack, Typography } from '@mui/material';
import { useDsTokens } from '../../theme/useDsTokens';
import { pwW, curA } from '../common/ruFormat';
import { phaseLabel } from '../../hooks/useUnitsConfig';
import { t } from '../../i18n';

export interface PhaseFlowLeg {
  ph: number;
  /** Signed W (+ import). */
  p: number | null;
  i?: number | null;
}

export interface PhaseFlowLine {
  id: string;
  name: string;
  /** units-config phase code: 1..3 one phase; 4 = 3-phase; 5–7 two-pole; 0/undefined = unset. */
  phase?: number;
  /** W as the channel reports it; null = no data. */
  w: number | null;
  /** Generation (PV) or storage (battery) on this line — not a consumer. */
  kind?: 'load' | 'gen' | 'storage';
  onOpen?: () => void;
}

export interface EnergyFlowPhasesProps {
  /** The input's total, W (signed). */
  gridW: number | null;
  legs: PhaseFlowLeg[];
  lines: PhaseFlowLine[];
  lost?: boolean;
}

/** A phase's rest: its measured P minus its single-phase lines (only when every line on it reads). */
export function phaseRest(leg: PhaseFlowLeg, lines: PhaseFlowLine[]): number | null {
  if (leg.p == null) return null;
  const own = lines.filter((l) => l.phase === leg.ph);
  // One sign for every point (root 16.10): P > 0 flows INTO the point. The input's phase P = Σ of its
  // points' P — a producing PV (P < 0) and a charging battery (P > 0) included — so the rest is
  // what remains whatever the kinds.
  if (own.some((l) => l.w == null)) return null;
  return leg.p - own.reduce((s, l) => s + (l.w as number), 0);
}

export const EnergyFlowPhases: React.FC<EnergyFlowPhasesProps> = ({ gridW, legs, lines, lost }) => {
  const ds = useDsTokens();
  const total = legs.reduce((s, l) => s + Math.max(0, l.p ?? 0), 0);
  const across = lines.filter((l) => l.phase != null && l.phase >= 4);
  const unassigned = lines.filter((l) => !l.phase);
  const num = { fontVariantNumeric: 'tabular-nums' as const, whiteSpace: 'nowrap' as const };
  const val = (w: number | null) => (w == null ? t('common.noData') : w < 0 ? `↑ ${pwW(-w)}` : pwW(w));

  // A source's figure by the one sign contract (root 16.10: P > 0 = into the point):
  // a battery P > 0 charging, P < 0 discharging; PV P < 0 producing «↑», P > 0 drawing (night
  // standby, or a hybrid inverter charging its battery from the grid).
  const srcVal = (l: PhaseFlowLine): string => {
    if (l.w == null) return t('common.noData');
    const a = Math.abs(l.w);
    if (a < 1) return pwW(0);
    if (l.kind === 'storage') return `${pwW(a)} · ${t(l.w > 0 ? 'flow.batteryCharging' : 'flow.batteryDischarging')}`;
    return l.w < 0 ? `↑ ${pwW(a)}` : `${pwW(a)} · ${t('flow.sourceDrawing')}`;
  };
  const lineRow = (l: PhaseFlowLine) => (
    <Stack key={l.id} direction="row" justifyContent="space-between" gap={1}
      onClick={l.onOpen} sx={{ cursor: l.onOpen ? 'pointer' : 'default', py: 0.25, minWidth: 0 }}>
      <Typography sx={{ fontSize: 13, color: ds.ink, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }} title={l.name}>{l.name}</Typography>
      <Typography sx={{ ...num, fontSize: 13, color: l.w == null ? ds.inkMuted : l.kind === 'gen' ? ds.solarInk : ds.ink }}>{l.kind === 'gen' || l.kind === 'storage' ? srcVal(l) : val(l.w)}</Typography>
    </Stack>
  );

  return (
    <Box sx={{ opacity: lost ? 0.6 : 1 }}>
      <Stack alignItems="center" sx={{ mb: 1.5 }}>
        <Typography sx={{ fontSize: 12, color: ds.inkMuted }}>{t('flow.grid')}</Typography>
        <Typography sx={{ ...num, fontSize: 18, fontWeight: 700, color: ds.ink }}>{gridW == null ? '—' : val(gridW)}</Typography>
      </Stack>
      <Box sx={{ display: 'grid', gap: 1.5, gridTemplateColumns: { xs: '1fr', sm: `repeat(${Math.max(1, legs.length)}, minmax(0, 1fr))` } }}>
        {legs.map((leg) => {
          const share = total > 0 && leg.p != null && leg.p > 0 ? (leg.p / total) * 100 : null;
          const rest = phaseRest(leg, lines);
          const own = lines.filter((l) => l.phase === leg.ph);
          return (
            <Box key={leg.ph} sx={{ border: `1px solid ${ds.border}`, borderRadius: 2, p: 1.5, minWidth: 0 }}>
              <Stack direction="row" justifyContent="space-between" alignItems="baseline" sx={{ mb: 0.5 }}>
                <Typography sx={{ fontWeight: 700, fontSize: 15 }}>L{leg.ph}</Typography>
                <Typography sx={{ ...num, fontWeight: 700, fontSize: 15 }}>{val(leg.p)}</Typography>
              </Stack>
              <Typography sx={{ ...num, fontSize: 12, color: ds.inkMuted, mb: 1 }}>
                {leg.i != null ? curA(leg.i) : ''}{share != null ? ` · ${t('flow.phaseShare', { pct: Math.round(share) })}` : ''}
              </Typography>
              {/* Hairline bar of the phase's share of the input — the imbalance at a glance. */}
              <Box sx={{ height: 4, borderRadius: 2, bgcolor: ds.surfaceRaised, mb: 1, overflow: 'hidden' }}>
                {share != null && <Box sx={{ height: '100%', width: `${share}%`, bgcolor: ds.primary }} />}
              </Box>
              {own.map(lineRow)}
              {rest != null && Math.abs(rest) >= 1 && (
                <Stack direction="row" justifyContent="space-between" gap={1} sx={{ py: 0.25 }}>
                  <Typography sx={{ fontSize: 13, color: ds.inkMuted }}>{t('flow.other')}</Typography>
                  <Typography sx={{ ...num, fontSize: 13, color: ds.inkMuted }}>{own.length || across.length ? '≈ ' : ''}{pwW(rest)}</Typography>
                </Stack>
              )}
            </Box>
          );
        })}
      </Box>
      {across.length > 0 && (
        <Box sx={{ mt: 1.5, border: `1px dashed ${ds.border}`, borderRadius: 2, p: 1.5 }}>
          <Typography sx={{ fontSize: 12, color: ds.inkMuted, mb: 0.5 }}>{t('flow.acrossPhases')}</Typography>
          {across.map((l) => lineRow({ ...l, name: `${l.name} · ${phaseLabel(l.phase as number)}` }))}
        </Box>
      )}
      {unassigned.length > 0 && (
        <Typography sx={{ mt: 1, fontSize: 12, color: ds.inkMuted }}>{t('flow.unassignedLines', { n: unassigned.length })}</Typography>
      )}
    </Box>
  );
};
