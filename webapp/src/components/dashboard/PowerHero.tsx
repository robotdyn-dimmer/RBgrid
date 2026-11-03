/**
 * PowerHero — the lead card of the Home overview (spec §4.1): input power now with a
 * direction arrow + label, voltage / frequency / house load, and a "% of house from solar" bar.
 *
 * Direction (arrow, "отдаём", solar share) shows only when the main input measures direction
 * (U3 / §14 W3). Values use ru-RU formatting and tabular figures (global, §2.1).
 */
import React from 'react';
import { Box, Stack, Typography, LinearProgress, Alert } from '@mui/material';
import { Card } from '@rbgrid/design-system';
import { useDsTokens } from '../../theme/useDsTokens';
import { FlowFrame, solarShare } from '../flow/flowFrame';
import { pw } from '../flow/flowHome';
import { PowerValue } from '../../economics/MoneyLens';
import { ChartLegend } from '../common/ChartLegend';
import { t, intlLocale } from '../../i18n';

const fmt = (v: number, d: number) => v.toLocaleString(intlLocale(), { minimumFractionDigits: d, maximumFractionDigits: d });

export interface PowerHeroProps {
  frame: FlowFrame;
  /** Main input present? An absent input reads «—», not «0 Вт». */
  present?: boolean;
  voltage?: number | null;
  /** Grid voltage per phase from the voltage modules (bus `ph[]`); shown under the mean. */
  phaseVoltages?: { ph: number; v: number }[];
  freq?: number | null;
  /** Any point measuring now? Without one the house load is unknown («—»), not 0. */
  anyPresent?: boolean;
  /** The input's phases disagree in sign with its total (Σ ph.p vs P) — a wiring/polarity hint. */
  phaseSignMismatch?: boolean;
  /** The input's value is held, not live (hub STALE / age_s): seconds since its last sample. */
  staleS?: number | null;
  /** No-flow threshold, W: from the input's resolution (p_res_w) when the hub says it, else 0,5 W (the hub's own floor). */
  idleW?: number;
}

const MiniStat: React.FC<{ label: string; value: string; sub?: string }> = ({ label, value, sub }) => (
  <Stack spacing={0.25}>
    <Typography sx={{ fontSize: 12, letterSpacing: '0.02em' }} color="text.secondary">{label}</Typography>
    <Typography sx={{ fontSize: 16, fontWeight: 600 }}>{value}</Typography>
    {sub && <Typography sx={{ fontSize: 12 }} color="text.secondary">{sub}</Typography>}
  </Stack>
);

export const PowerHero: React.FC<PowerHeroProps> = ({ frame, voltage, phaseVoltages, freq, present = true, anyPresent = true, phaseSignMismatch = false, staleS = null, idleW = 5 }) => {
  const ds = useDsTokens();
  // |P| under 5 W is no flow — no «берём ↓ 0 Вт» (UX audit INP-23/GEN-18); an absent input has
  // no direction at all (INP-03).
  const idle = present && Math.abs(frame.grid) * 1000 < idleW;
  const exporting = frame.grid < 0 && frame.gridHasDirection && !idle;
  // A held value is greyed and says how old it is (SCADA-R04: frozen figures looked live).
  const heroColor = staleS != null ? ds.inkMuted : exporting ? ds.flowExport : ds.ink;
  const label = !present ? t('cards.heroInputNoData')
    : idle ? t('cards.heroIdle')
      : frame.gridHasDirection ? (exporting ? t('cards.heroExporting') : t('cards.heroImporting')) : t('cards.heroInputNow');
  const arrow = present && !idle && frame.gridHasDirection ? (exporting ? '↑ ' : '↓ ') : '';
  const sharePct = Math.round(solarShare(frame) * 100);
  const showShare = frame.gridHasDirection && frame.solar != null && frame.house > 0;

  return (
    <Card>
      <Stack spacing={1.5} sx={{ py: 0.5 }}>
        <Typography sx={{ fontSize: 12, letterSpacing: '0.02em' }} color="text.secondary">
          {label}{staleS != null && <Box component="span" sx={{ color: ds.sevWarning, fontWeight: 600 }}>{` · ${t('cards.staleFor', { s: Math.round(staleS) })}`}</Box>}
        </Typography>
        {/* displayXl 52 desktop / 44 phone (spec §2, review 03 P1-12); never wraps the unit. */}
        {/* Under the «Мощность ⇄ Деньги» lens: money mode leads with ≈ ₽/ч (export earns). */}
        <PowerValue w={present ? frame.grid * 1000 : null} size="hero" prefix={arrow} color={heroColor} />
        {/* Diagnostic only — the polarity itself is hardware/rbAmp firmware, never a UI setting
            (operator 2026-10-15). UX audit T5. */}
        {present && (frame.exportWithoutSource || phaseSignMismatch) && (
          <Alert severity="warning" sx={{ py: 0 }}>
            {frame.exportWithoutSource ? t('cards.exportWithoutSource') : t('cards.phaseSignMismatch')}
          </Alert>
        )}

        <Stack direction="row" columnGap={4} rowGap={1.5} flexWrap="wrap" useFlexGap sx={{ pt: 0.5 }}>
          <MiniStat label={t('cards.voltage')} value={voltage != null ? `${fmt(voltage, 0)} ${t('units.V')}` : '—'}
            sub={phaseVoltages?.length ? phaseVoltages.map((p) => `L${p.ph} ${fmt(p.v, 0)}`).join(' · ') : undefined}
          />
          <MiniStat label={t('cards.frequency')} value={freq != null ? `${fmt(freq, 2)} ${t('units.Hz')}` : '—'} />
          <MiniStat label={t('cards.houseLoad')} value={anyPresent && frame.houseKnown !== false ? pw(frame.house) : '—'} />
        </Stack>

        {showShare && (
          <Box sx={{ pt: 0.5 }}>
            <Stack direction="row" justifyContent="space-between" sx={{ mb: 0.5 }}>
              <Typography sx={{ fontSize: 12 }} color="text.secondary">{t('cards.houseFromSolar')}</Typography>
              <Typography sx={{ fontSize: 12, fontWeight: 600, color: ds.solarInk }}>{sharePct}%</Typography>
            </Stack>
            <LinearProgress
              variant="determinate"
              value={sharePct}
              aria-label={t('cards.houseFromSolar')}
              sx={{
                height: 6,
                borderRadius: 999,
                backgroundColor: ds.flowImport,
                '& .MuiLinearProgress-bar': { backgroundColor: ds.solar },
              }}
            />
            <ChartLegend sx={{ mt: 0.75 }} items={[
              { label: t('legend.fromSolar'), color: ds.solar },
              { label: t('legend.fromGrid'), color: ds.flowImport },
            ]} />
          </Box>
        )}
      </Stack>
    </Card>
  );
};
