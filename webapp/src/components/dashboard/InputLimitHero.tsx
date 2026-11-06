/**
 * InputLimitHero — the Предприятие primary block «Ввод против лимита» (spec §4.2.1, review 04
 * §5.3 #1, persona 07: «хватит ли ввода и что не учтено»).
 *
 * Input P (XL) against the input limit (main unit `rated_power_w` → `join.input.ratedPowerW`)
 * as a bar + percent + «запас N кВт»; frequency; the most loaded phase current vs the breaker
 * (`max_current_a`); and the unmetered «Остаток» (input − Σ lines) as a visible figure.
 *
 * Data truth (U3): a limit that isn't set is NEVER substituted — the block says «лимит ввода
 * не задан» and links to the settings. Input not present → «—» + «нет связи», never «0 Вт».
 */
import React from 'react';
import { Link as RouterLink } from 'react-router-dom';
import { Box, Stack, Typography, Link } from '@mui/material';
import { Card } from '@rbgrid/design-system';
import { useDsTokens } from '../../theme/useDsTokens';
import { PhaseLoad } from '../../pages/overviewData';
import { pwW, nf, curA, freqHz } from '../common/ruFormat';
import { useLensFormat } from '../../economics/MoneyLens';
import { t } from '../../i18n';

export interface InputLimitHeroProps {
  /** Signed input power (W); null = not measured / not present. */
  powerW: number | null;
  /** W9: `powerW` is the ×N balance estimate of a single-CT multi-phase input → «≈». */
  estimated?: boolean;
  /** Whether the input unit is present (on the bus). */
  present: boolean;
  /** Input limit (W) — undefined = not set. */
  limitW?: number;
  /** Input breaker rating (A) — undefined = not set. */
  breakerA?: number;
  freq: number | null;
  phases: PhaseLoad[] | null;
  /** Unmetered load (W) = input − Σ lines; null when the input isn't measured. */
  unmeteredW: number | null;
  /** Frame lost (no connection) → last values greyed. */
  lost?: boolean;
  /** Where «задать лимит» leads. */
  settingsHref?: string;
  /** What the breaker passes (A × V × conductors), W — a limit well below it is questioned. */
  breakerW?: number;
  /** units-config has arrived: until then «not set» would be a guess (3φ audit I3P-04). */
  configLoaded?: boolean;
}

const kw = (w: number): string => `${nf(w / 1000, w >= 100_000 ? 0 : 1)} ${t('units.kW')}`;

const REM_PCT_FLOOR_W = 50;

const remPctOf = (unmeteredW: number | null | undefined, powerW: number | null | undefined): number =>
  unmeteredW != null && powerW ? Math.abs((unmeteredW / Math.max(1, Math.abs(powerW))) * 100) : 0;

export const InputLimitHero: React.FC<InputLimitHeroProps> = ({
  powerW, estimated, present, limitW, breakerA, freq, phases, unmeteredW, lost, settingsHref = '/devices', breakerW, configLoaded = true,
}) => {
  const ds = useDsTokens();
  const lens = useLensFormat();
  const has = present && powerW != null;
  // «Мощность ⇄ Деньги»: money mode leads with ≈ ₽/ч; the limit bar and headroom stay physical
  // (a limit is kW), so the measured kW moves next to «из N кВт».
  const lv = has ? lens.power(powerW, false) : null;
  const moneyLead = !!lv?.money;
  const unm = unmeteredW == null ? null : lens.power(unmeteredW, false);
  const exporting = has && (powerW as number) < 0;
  const load = has ? Math.max(0, powerW as number) : null;
  const pct = load != null && limitW ? (load / limitW) * 100 : null;
  const word = pct == null ? null : pct > 100 ? t('cards.limitAbove') : pct > 92 ? t('cards.limitNear') : null;
  const sev = pct != null && pct > 100 ? ds.sevCritical : ds.sevWarning;
  const barColor = word ? sev : ds.primary;

  // Most loaded phase (estimate from lines) vs the breaker.
  const withData = (phases ?? []).filter((p) => p.hasData);
  const maxPhase = withData.length ? withData.reduce((m, p) => (p.currentA > m.currentA ? p : m)) : null;
  const phasePct = maxPhase && breakerA ? (maxPhase.currentA / breakerA) * 100 : null;
  const phaseWord = phasePct == null ? null : phasePct > 100 ? t('cards.phaseOverload') : phasePct > 80 ? t('cards.phaseHigh') : null;
  // From lines that cover little of the input, the busiest phase is a LOWER bound («≥») — INP-07.
  const phaseIncomplete = !!maxPhase && !maxPhase.measured && remPctOf(unmeteredW, powerW) > 20;

  // Unmetered share of the input; > 10 % is the integrator's warning.
  // A near-idle input (a few W) gives a meaningless share — no percent, no warning.
  const remPct = unmeteredW != null && load != null && load >= REM_PCT_FLOOR_W ? (unmeteredW / load) * 100 : null;
  const remWarn = remPct != null && Math.abs(remPct) > 10;

  const num = { fontVariantNumeric: 'tabular-nums' as const };
  const label = { fontSize: 12, letterSpacing: '0.02em', color: ds.inkMuted };

  const Stat: React.FC<{ title: string; value: string; sub?: React.ReactNode; color?: string }> = ({ title, value, sub, color }) => (
    <Stack spacing={0.25} sx={{ minWidth: 0 }}>
      <Typography sx={label}>{title}</Typography>
      <Typography sx={{ ...num, fontSize: 20, fontWeight: 600, color: color ?? ds.ink, lineHeight: 1.2 }}>{value}</Typography>
      {sub && <Typography component="div" sx={{ fontSize: 12, color: color ?? ds.inkMuted }}>{sub}</Typography>}
    </Stack>
  );

  return (
    <Card>
      <Box
        aria-label={t('cards.limitTitle')}
        sx={{
          display: 'grid', gap: { xs: 2, md: 3 }, py: 0.5, opacity: lost ? 0.6 : 1,
          gridTemplateColumns: { xs: '1fr', lg: 'minmax(0, 7fr) minmax(0, 5fr)' },
        }}
      >
        {/* Primary: input P vs the limit */}
        <Stack spacing={1.25} sx={{ minWidth: 0 }}>
          <Typography sx={{ fontWeight: 700, fontSize: 15 }}>{t('cards.limitTitle')}</Typography>
          <Stack direction="row" alignItems="baseline" spacing={1.5} flexWrap="wrap" useFlexGap>
            <Typography title={has && estimated ? t('cards.balanceEstimate') : undefined} sx={{ ...num, fontSize: { xs: 36, md: 44 }, fontWeight: 600, letterSpacing: '-0.02em', lineHeight: 1.05, color: has ? ds.ink : ds.inkMuted }}>
              {has && estimated ? '≈ ' : ''}{has ? (moneyLead ? lv?.main : pwW(Math.abs(powerW as number))) : '—'}
            </Typography>
            {!present && <Typography sx={{ fontSize: 14, fontWeight: 600, color: ds.statusOffline }}>○ {t('cards.offline')}</Typography>}
            {exporting && <Typography sx={{ fontSize: 14, color: ds.flowExport }}>↑ {t('cards.exportingToGrid')}</Typography>}
            {(limitW != null || moneyLead) && (
              <Typography sx={{ ...num, fontSize: 15, color: ds.inkMuted }}>
                {moneyLead ? pwW(Math.abs(powerW as number)) : ''}{moneyLead && limitW != null ? ' ' : ''}
                {limitW != null ? t('cards.limitOf', { limit: kw(limitW) }) : ''}
              </Typography>
            )}
          </Stack>

          {limitW != null ? (
            <Box>
              <Box
                role="img"
                aria-label={pct != null ? t('cards.limitBarAria', { pct: nf(pct), limit: kw(limitW) }) : t('cards.limitBarAriaNoData')}
                sx={{ height: 12, borderRadius: 6, bgcolor: ds.surfaceRaised, overflow: 'hidden' }}
              >
                {pct != null && <Box sx={{ height: '100%', width: `${Math.min(100, pct)}%`, bgcolor: barColor, borderRadius: 6 }} />}
              </Box>
              <Stack direction="row" justifyContent="space-between" flexWrap="wrap" useFlexGap columnGap={2} sx={{ mt: 0.75 }}>
                <Typography sx={{ ...num, fontSize: 13, fontWeight: word ? 600 : 400, color: word ? sev : ds.inkMuted }}>
                  {pct != null ? `${t('cards.limitPct', { pct: nf(pct) })}${word ? ` · ${word}` : ''}` : '—'}
                </Typography>
                <Typography sx={{ ...num, fontSize: 13, fontWeight: 600, color: ds.ink }}>
                  {load != null ? t('cards.headroom', { value: kw(Math.max(0, limitW - load)) }) : t('cards.headroom', { value: '—' })}
                </Typography>
              </Stack>
              {/* The same rule as the point page (UnitParamsPanel): a limit under half of what the
                  breaker passes is questioned on the Overview too (3φ audit I3P-01: «of 11.5 kW» on a
                  3 × 50 A input that passes ≈ 34.5 kW — a single-phase figure). */}
              {breakerW != null && limitW < breakerW * 0.5 && (
                <Typography sx={{ fontSize: 12, color: ds.sevWarning, mt: 0.5 }}>
                  {t('cards.limitBelowBreaker', { limit: kw(limitW), breaker: kw(breakerW) })}{' '}
                  <Link component={RouterLink} to={settingsHref} underline="hover">{t('cards.limitCheck')}</Link>
                </Typography>
              )}
            </Box>
          ) : (
            !configLoaded ? <Typography sx={{ fontSize: 13, color: ds.inkMuted }}>{t('cards.limitLoading')}</Typography> :
            <Typography sx={{ fontSize: 13, color: ds.inkMuted }}>
              {t('cards.limitNotSet')}{' '}
              <Link component={RouterLink} to={settingsHref} underline="hover">{t('cards.limitNotSetLink')}</Link>
              {t('cards.limitNotSetTail')}
            </Typography>
          )}
        </Stack>

        {/* Context: f · most loaded phase · unmetered */}
        <Box
          sx={{
            display: 'grid', gap: 2, alignContent: 'start', minWidth: 0,
            gridTemplateColumns: { xs: 'repeat(2, minmax(0, 1fr))', md: 'repeat(3, minmax(0, 1fr))' },
            pt: { lg: 3.5 },
          }}
        >
          <Stat
            title={t('cards.unmetered')}
            value={unm ? (unm.money ? unm.main : pwW(unmeteredW)) : '—'}
            color={remWarn ? ds.sevWarning : undefined}
            sub={`${unm?.money ? `${pwW(unmeteredW)} · ` : ''}${remPct != null ? t('cards.unmeteredPct', { pct: nf(Math.abs(remPct)) }) : t('cards.unmeteredFormula')}`}
          />
          <Stat
            title={t('cards.maxPhase')}
            value={maxPhase ? `L${maxPhase.phase} · ${phaseIncomplete ? '≥ ' : ''}${curA(maxPhase.currentA)}` : '—'}
            color={phaseWord ? ds.sevAlarm : undefined}
            sub={breakerA
              ? (phasePct != null ? `${phaseIncomplete ? '≥ ' : ''}${t('cards.breakerPct', { pct: nf(phasePct), breaker: nf(breakerA) })}${phaseWord ? ` · ${phaseWord}` : ''}${phaseIncomplete ? ` · ${t('cards.phaseLowerBound')}` : ''}` : t('cards.breaker', { breaker: nf(breakerA) }))
              : t('cards.breakerNotSet')}
          />
          <Stat title={t('cards.frequency')} value={freqHz(freq)} />
        </Box>
      </Box>
    </Card>
  );
};
