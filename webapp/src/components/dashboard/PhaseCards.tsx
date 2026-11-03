/**
 * PhaseCards — the Предприятие «Пульт» phases L1–L3 (spec §4.2.1, review 04 §5.3 / P1-12).
 *
 * ONE card with a horizontal load bar per phase against the input breaker rating
 * (`max_current_a`, G4); >80 % → sev-alarm, >100 % → sev-critical, always with a word (§9).
 * Figures are MEASURED on the input when it carries `ph[]` (U3I3, root delta №4) — then U and
 * cos φ per phase and the current unbalance are shown too; otherwise they are AGGREGATED from
 * the lines assigned to each phase (`phaseLoads`, «оценка по линиям»), and U / cos φ are NOT
 * drawn (aggregation can't give them) — no rows of «—».
 * Rendered only when at least one line carries a phase assignment.
 * The rows are the mains system's phases (`grid`): L1–L3 on a three-phase site, legs L1/L2 on a
 * US split-phase service (120/240 V; a two-pole 240 V line loads both legs).
 */
import React from 'react';
import { Box, Stack, Typography } from '@mui/material';
import { Card } from '@rbgrid/design-system';
import { useDsTokens } from '../../theme/useDsTokens';
import { UnitEntityGroup } from '../../entities/model';
import { UnitsConfigJoin } from '../../hooks/useUnitsConfig';
import { phaseLoads, phaseImbalancePct, linesCoverage, PhaseLoad, mainUnit } from '../../pages/overviewData';
import { pwW, curA, nf, volt, qCharacter } from '../common/ruFormat';
import { GridSystem, voltageBand } from '../../entities/gridSystem';
import { t } from '../../i18n';

export interface PhaseCardsProps {
  groups: UnitEntityGroup[];
  join: UnitsConfigJoin;
  /** Frame lost (no connection) → values greyed. */
  lost?: boolean;
  /** Mains system; absent → derived from the points' phases (1P unless something is on L2/L3). */
  grid?: GridSystem;
  /** Nominal L-N voltage: a measured phase outside ±10 % is flagged. Absent → not checked. */
  nominalV?: number;
  /** Phases the input has reported per-phase this session: a frame without ph[] (stale) keeps
   *  these rows as «нет данных» instead of hiding the card (when there's no line estimate). */
  measuredLegs?: number[];
}

/** Current unbalance above this is flagged (loads worth redistributing between phases). */
const IMBALANCE_HIGH_PCT = 20;

export const PhaseCards: React.FC<PhaseCardsProps> = ({ groups, join, lost, grid, nominalV, measuredLegs }) => {
  const ds = useDsTokens();
  const breakerA = join.input.maxCurrentA;
  const phases = phaseLoads(groups, join, grid)
    ?? (measuredLegs?.length
      ? Array.from(new Set([...(grid?.legs ?? []), ...measuredLegs])).sort((a, b) => a - b)
        .map((phase) => ({ phase, powerW: 0, currentA: 0, lines: 0, hasData: false, measured: true }))
      : null);
  const split = grid?.kind === 'SPLIT';
  if (!phases) return null;
  const measured = phases.some((p) => p.measured);
  const coverage = linesCoverage(groups, join);
  const imbalance = phaseImbalancePct(phases);
  const imbalanceHigh = imbalance != null && imbalance > IMBALANCE_HIGH_PCT;
  const voltLine = (() => {
    if (!measured || !nominalV) return null;
    const vs = (phases as PhaseLoad[]).filter((p) => p.voltageV != null && (p.voltageV as number) > 1);
    if (vs.length < 2) return null;
    const vals = vs.map((p) => p.voltageV as number);
    const mean = vals.reduce((a, x) => a + x, 0) / vals.length;
    const out = vs.filter((p) => voltageBand(p.voltageV, nominalV)).map((p) => `L${p.phase}`).join(', ');
    return { lo: nominalV * 0.9, hi: nominalV * 1.1, out, unb: ((Math.max(...vals) - Math.min(...vals)) / mean) * 100 };
  })();

  const num = { fontVariantNumeric: 'tabular-nums' as const, whiteSpace: 'nowrap' as const };
  // Phase rotation from the 3-phase input module (root 9bc08b40): reverse spins 3-phase motors
  // backwards, abnormal = the angles aren't ~120° apart (a phase missing or miswired).
  const seq = measured && !split ? mainUnit(groups, join)?.phaseSeq : undefined;
  // the U · cos φ column widens when it also carries the angle («231 В ∠ −119,8° · cos φ 0,97 инд.»)
  const uCol = phases.some((p) => (p as PhaseLoad).angle != null && p.phase !== 1) ? '230px' : '130px';
  const fmtAngle = (a: number) => `∠ ${a > 0 ? '+' : a < 0 ? '−' : ''}${nf(Math.abs(a), 1)}°`;

  return (
    <Card>
      <Stack spacing={1.25} sx={{ py: 0.5, opacity: lost ? 0.6 : 1 }}>
        <Stack direction="row" alignItems="baseline" justifyContent="space-between" flexWrap="wrap" useFlexGap columnGap={2}>
          <Typography sx={{ fontWeight: 700, fontSize: 15 }}>{split ? t('cards.legsTitle') : t('cards.phasesTitle')}</Typography>
          <Typography sx={{ fontSize: 12, color: ds.inkMuted }}>
            {split && grid ? `${nf(grid.nominalV)}/${nf(grid.lineV)} ${t('units.V')} · ` : ''}{measured ? t('cards.phasesMeasured') : t('cards.phasesEstimate')}{breakerA ? ` · ${phases.length > 1 ? t('cards.breakerPoles', { n: phases.length, breaker: nf(breakerA) }) : t('cards.breaker', { breaker: nf(breakerA) })}` : join.loaded ? ` · ${t('cards.breakerNotSet')}` : ''}
          </Typography>
        </Stack>
        {/* An estimate from lines covers only what the lines measure (INP-07: 19 % of the input in
            the demo, «27 % автомата» read as fact) — say how much. */}
        {!measured && coverage != null && coverage < 0.8 && (
          <Typography sx={{ fontSize: 12, color: ds.sevWarning }}>
            {t('cards.phasesCoverage', { pct: nf(Math.max(0, coverage) * 100) })}
          </Typography>
        )}
        {imbalance != null && (
          <Typography sx={{ ...num, fontSize: 12, color: imbalanceHigh ? ds.sevAlarm : ds.inkMuted, fontWeight: imbalanceHigh ? 600 : 400 }}>
            {t('cards.phaseImbalance', { pct: nf(imbalance) })}{imbalanceHigh ? ` · ${t('cards.phaseHigh')}` : ''}
          </Typography>
        )}
        {/* The rule said where it hides (3φ audit L3P-17): under 1 A a phase's % swings with a lamp. */}
        {imbalance == null && measured && phases.filter((p) => p.hasData).length > 1 && (
          <Typography sx={{ fontSize: 12, color: ds.inkMuted }}>{t('cards.phaseImbalanceLow')}</Typography>
        )}
        {/* Voltage at a glance for a shift check (I3P-02): all phases within ±10 % of nominal and the
            voltage unbalance (max − min over the mean). */}
        {voltLine && (
          <Typography sx={{ ...num, whiteSpace: 'normal', fontSize: 12, color: voltLine.out ? ds.sevAlarm : ds.inkMuted, fontWeight: voltLine.out ? 600 : 400 }}>
            {voltLine.out
              ? t('cards.voltOut', { phases: voltLine.out, lo: nf(voltLine.lo), hi: nf(voltLine.hi) })
              : t('cards.voltIn', { lo: nf(voltLine.lo), hi: nf(voltLine.hi) })}
            {` · ${t('cards.voltUnbalance', { pct: nf(voltLine.unb, 1) })}`}
          </Typography>
        )}
        {seq && (
          <Typography sx={{ fontSize: 12, color: seq === 'forward' ? ds.inkMuted : ds.sevAlarm, fontWeight: seq === 'forward' ? 400 : 600 }}>
            {t(`cards.phaseSeq_${seq}`)}
          </Typography>
        )}
        {phases.map((a) => {
          const loadPct = breakerA && a.hasData ? (a.currentA / breakerA) * 100 : null;
          const word = loadPct == null ? null : loadPct > 100 ? t('cards.phaseOverload') : loadPct > 80 ? t('cards.phaseHigh') : null;
          const sev = loadPct != null && loadPct > 100 ? ds.sevCritical : ds.sevAlarm;
          const barColor = loadPct == null ? ds.border : word ? sev : ds.primary;
          return (
            <Box
              key={a.phase}
              sx={{
                display: 'grid', alignItems: 'center', columnGap: 1.5, rowGap: 0.5,
                gridTemplateColumns: { xs: '28px 1fr auto', md: measured ? `28px 90px 70px ${uCol} 1fr 110px` : '28px 90px 70px 1fr 110px' },
              }}
            >
              <Typography sx={{ fontWeight: 700, fontSize: 14 }}>L{a.phase}</Typography>
              <Typography sx={{ ...num, fontSize: 15, color: ds.ink, gridColumn: { xs: 'auto', md: 'auto' } }}>
                {a.hasData && !a.noPower ? (a.powerW < 0
                  ? <Box component="span" sx={{ color: ds.flowExport }}>↑ {pwW(-a.powerW)}</Box>
                  : pwW(a.powerW)) : '—'}
              </Typography>
              <Typography sx={{ ...num, fontSize: 14, color: ds.inkMuted, textAlign: { xs: 'right', md: 'left' } }}>
                {a.hasData ? curA(a.currentA) : '—'}
              </Typography>
              {measured && (() => {
                const vb = nominalV ? voltageBand(a.voltageV, nominalV) : null;
                return (
                  <Typography sx={{ ...num, gridColumn: { xs: '1 / -1', md: 'auto' }, fontSize: 12, color: ds.inkMuted }}>
                    {a.voltageV != null && (
                      <Box component="span" sx={vb ? { color: ds.sevAlarm, fontWeight: 600 } : undefined}>
                        {volt(a.voltageV)}{vb ? ` ${t(vb === 'low' ? 'cards.voltLow' : 'cards.voltHigh')}` : ''}
                      </Box>
                    )}
                    {a.voltageV != null && a.angle != null && a.phase !== 1 ? ` ${fmtAngle(a.angle)}` : ''}
                    {a.voltageV != null && a.pf != null ? ' · ' : ''}
                    {a.pf != null ? `${t('units.pf')} ${nf(a.pf, 2)}${qCharacter(a.q, a.powerW) ? ` ${qCharacter(a.q, a.powerW)}` : ''}` : ''}
                    {a.voltageV == null && a.pf == null ? '—' : ''}
                  </Typography>
                );
              })()}
              <Box sx={{ gridColumn: { xs: '1 / -1', md: 'auto' }, height: 8, borderRadius: 4, bgcolor: ds.surfaceRaised, overflow: 'hidden' }}>
                {loadPct != null && (
                  <Box sx={{ height: '100%', width: `${Math.min(100, loadPct)}%`, bgcolor: barColor, borderRadius: 4 }} />
                )}
              </Box>
              <Typography sx={{ ...num, gridColumn: { xs: '1 / -1', md: 'auto' }, fontSize: 12, textAlign: { md: 'right' }, color: word ? sev : ds.inkMuted, fontWeight: word ? 600 : 400 }}>
                {loadPct != null ? `${t('cards.phaseBreakerPct', { pct: loadPct > 0 && loadPct < 0.5 ? '<1' : nf(loadPct) })}${word ? ` · ${word}` : ''}` : a.hasData ? (join.loaded ? t('cards.phaseBreakerNotSet') : '…') : a.measured ? t('cards.phaseNoReading') : t('cards.phaseNoData', { lines: a.lines })}
              </Typography>
            </Box>
          );
        })}
      </Stack>
    </Card>
  );
};
