/**
 * OverviewEnergyChart — «Сегодня по часам» (spec §4.1.3): diverging hourly energy bars,
 * import UP, export DOWN, the current hour hatched, on a single kWh axis. Plain inline SVG
 * (no chart.js) so it matches the flow schemas and stays token-driven. Reduced-motion safe
 * (static). ru-RU ticks; «—» / empty state when there's no data.
 *
 * With a generation point (energy mode, operator 20.10): the bar is the HOUSE's hour, in two
 * sections — from the grid (the input's import) and from its own solar (the generation's output
 * minus what went out to the grid); export stays below the line. So a sunny day shows as a tall
 * bar with a solar section, and the house's use is the bar's height. Money mode stays import ↑ /
 * export ↓ — what was paid and earned.
 *
 * «Мощность ⇄ Деньги»: in money mode the bars are the hour's money — the device's own
 * per-hour cost/revenue (history `economics` c/r) when it has them, else kWh × the live rate (≈).
 *
 * Fixed 200 px height, drawn in REAL pixels for the measured card width (`viewBox = 0 0 W H`)
 * so the 11 px axis labels stay 11 px at every width (review 03 P0-8: they were 23 px on
 * 1920 and 6.5 px on 768 when the 760-wide viewBox was scaled). Hour labels thin out so
 * they keep ≥ 28 px apart.
 */
import React, { useMemo } from 'react';
import { Typography, Box, Link } from '@mui/material';
import { Link as RouterLink } from 'react-router-dom';
import { historyHref } from '../../pages/historyLink';
import { Card } from '@rbgrid/design-system';
import { useDsTokens } from '../../theme/useDsTokens';
import { useHistory } from '../../hooks/useHistory';
import { HistoryEnergyRecord, HistoryEconomicsRecord } from '../../types/history';
import { useLensFormat } from '../../economics/MoneyLens';
import { MAIN_UNIT_KEY } from '../flow/flowFrame';
import { useElementWidth } from '../../hooks/useElementWidth';
import { ChartLegend } from '../common/ChartLegend';
import { HoverLayer } from '../charts/HoverLayer';
import { t, intlLocale } from '../../i18n';
import { WALLCLOCK_TS } from '../../entities/history';
import { useDeviceClock } from '../../core/deviceTime';

/** ec/es are ALWAYS float Wh on the wire (schema units_of_measure.energy, canon fc0f962b). */
const toKwh = (wh: number): number => wh / 1000;
const nf = (v: number, d: number) => v.toLocaleString(intlLocale(), { minimumFractionDigits: d, maximumFractionDigits: d });

export interface OverviewEnergyChartProps {
  unitId?: string;
  /** Deprecated (kept for callers): the export legend now follows the data, not the role. */
  hasGeneration?: boolean;
  /** The input's live totalizers (Wh): the running hour = these − the last closed record's
   *  `et_in` / `et_out` (L3P-05: the legend promised a hatched current hour that never came). */
  liveEIn?: number | null;
  liveEOut?: number | null;
  /** History id of the site's generation point (its hour `es` = what it produced); with it the
   *  energy view splits the house's hour into grid + own solar. One point: several are rare and
   *  their sum would need a request per point. */
  genUnitId?: string | null;
  /** That point's live output totalizer (Wh) — the running hour's solar. */
  liveGenOut?: number | null;
}

const H = 200;
/** Hour-label strides; the smallest keeping labels ≥ 28 px apart wins. */
const HOUR_STRIDES = [1, 2, 3, 4, 6, 12];
const PAD_L = 8;
const PAD_R = 8;
const PAD_T = 14;
const PAD_B = 22;

export const OverviewEnergyChart: React.FC<OverviewEnergyChartProps> = ({ unitId = MAIN_UNIT_KEY, liveEIn, liveEOut, genUnitId, liveGenOut }) => {
  const ds = useDsTokens();
  const lens = useLensFormat();
  // House view: energy mode with a generation point on site.
  const solar = !lens.moneyMode && !!genUnitId;
  const gen = useHistory(solar ? genUnitId ?? null : null, 'hour', 'energy');
  const { records, loading: histLoading, error: histError } = useHistory(unitId, 'hour', 'energy');
  const eco = useHistory(lens.moneyMode ? unitId : null, 'hour', 'economics');
  // Yesterday's hours for the comparison caps (charts audit A: «cheaper than yesterday by 44 %»
  // in Savings had no picture).
  const clockY = useDeviceClock();
  const yRange = useMemo(() => {
    const d0 = clockY.dayStart(Math.floor(Date.now() / 60_000) * 60);
    return { from: clockY.dayStart(d0 - 43_200), to: d0 };
  }, [clockY]);
  const yE = useHistory(unitId, 'hour', 'energy', 30, yRange);
  const yG = useHistory(solar ? genUnitId ?? null : null, 'hour', 'energy', 30, yRange);
  const yM = useHistory(lens.moneyMode ? unitId : null, 'hour', 'economics', 30, yRange);
  const [measureRef, measured] = useElementWidth<HTMLDivElement>(760);
  const W = Math.max(240, measured);

  const clock = useDeviceClock();
  // TODAY in the hub's time, 24 hour slots 0..23 (UX audit HOME-09/GEN-08/INP-10/11: it used to be
  // «the last 24 records» — yesterday's evening, a doubled «16» after a reboot, gaps squeezed out).
  // Records of one hour (a reboot splits it: 16:00 + 16:17) are summed into that slot; an hour
  // without a record stays empty.
  const bars = useMemo(() => {
    const now = Math.floor(Date.now() / 1000);
    const day0 = clock.dayStart(now);
    const recs = (records as HistoryEnergyRecord[]).filter((r) => typeof r.ts === 'number' && r.ts >= WALLCLOCK_TS && r.ts >= day0 && r.ts < day0 + 86400);
    const money = new Map<number, HistoryEconomicsRecord>();
    if (lens.moneyMode) (eco.records as HistoryEconomicsRecord[]).forEach((e) => money.set(e.ts, e));
    const { buy, sell } = lens.rate;
    const slots = Array.from({ length: 24 }, (_, h) => ({ hour: h, imp: 0, exp: 0, gen: 0, partial: false, has: false, dur: 0, durKnown: true }));
    // The generation point's hours (what it produced = its `es`); under 10 Wh is noise.
    const genRecs = solar ? (gen.records as HistoryEnergyRecord[]).filter((r) => typeof r.ts === 'number' && r.ts >= WALLCLOCK_TS && r.ts >= day0 && r.ts < day0 + 86400) : [];
    for (const r of genRecs) { const es = r.es ?? 0; if (es >= 10) slots[clock.parts(r.ts).h].gen += toKwh(es); }
    for (const r of recs) {
      const s = slots[clock.parts(r.ts).h];
      const e = money.get(r.ts);
      const imp = toKwh(r.ec ?? 0);
      // Export under 10 Wh an hour is metering noise, not a flow (INP-R01: 0,001–0,005 Wh drawn as
      // tall «Отдано» bars on their own scale).
      const exp = (r.es ?? 0) < 10 ? 0 : toKwh(r.es ?? 0);
      s.imp += lens.moneyMode ? (e ? e.c : imp * (buy ?? 0)) : imp;
      s.exp += lens.moneyMode ? (e ? e.r : exp * (sell ?? 0)) : exp;
      s.partial = s.partial || ((r.q ?? 0) & 1) === 1;
      if (typeof r.dur === 'number') s.dur += r.dur; else s.durKnown = false;
      s.has = true;
    }
    // A reboot splits an hour into two «partial» records (.110 16.10: 3121 s + 475 s, a 4 s gap) —
    // together they cover it; the slot is partial only when its records miss over a minute.
    for (const s of slots) if (s.has && s.durKnown && s.dur > 0) s.partial = s.dur < 3540;
    // The running hour from the live totalizers against the last closed record's totalizer —
    // only when that record closes at this hour's start (no gap to smear in), hatched as current.
    const last = recs.reduce<HistoryEnergyRecord | null>((m, r) => (!m || r.ts > m.ts ? r : m), null);
    const hourStart = now - (now % 3600);
    const lastEnd = last ? last.ts + (last.dur ?? 3600) : 0;
    // The biggest closed hour bounds a plausible running one: three times it is a counter reset or a
    // mismatch, not consumption (charts audit O2: 82 kWh «this hour» beside ≈ 5 kWh a day).
    const closedMax = Math.max(0, ...slots.filter((x) => x.has).map((x) => x.imp));
    if (last && liveEIn != null && typeof last.et_in === 'number' && Math.abs(lastEnd - hourStart) <= 120) {
      const s = slots[clock.parts(now).h];
      const imp = Math.max(0, toKwh(liveEIn - last.et_in));
      const implausible = closedMax > 0 && !lens.moneyMode && imp > closedMax * 3;
      const expWh = liveEOut != null && typeof last.et_out === 'number' ? Math.max(0, liveEOut - last.et_out) : 0;
      const exp = expWh < 10 ? 0 : toKwh(expWh);
      if (!s.has && !implausible && !(lens.moneyMode && closedMax > 0 && imp * (lens.rate.buy ?? 0) > closedMax * 3)) {
        s.imp = lens.moneyMode ? imp * (lens.rate.buy ?? 0) : imp;
        s.exp = lens.moneyMode ? exp * (lens.rate.sell ?? 0) : exp;
        s.has = true;
        s.partial = true;
        // The running hour's solar: its live output against its last closed record.
        const lastGen = genRecs.reduce<HistoryEnergyRecord | null>((m, r) => (!m || r.ts > m.ts ? r : m), null);
        if (solar && lastGen && liveGenOut != null && typeof lastGen.et_out === 'number'
          && Math.abs(lastGen.ts + (lastGen.dur ?? 3600) - hourStart) <= 120) {
          const g = Math.max(0, liveGenOut - lastGen.et_out);
          s.gen = g < 10 ? 0 : toKwh(g);
        }
      }
    }
    return recs.length ? slots : [];
  }, [records, eco.records, gen.records, solar, lens.moneyMode, lens.rate, clock, liveEIn, liveEOut, liveGenOut]);
  /** The house's own solar in an hour: produced − sent to the grid (never below 0). */
  const selfOf = (b: { gen: number; exp: number }): number => (solar ? Math.max(0, b.gen - b.exp) : 0);
  const yday = useMemo(() => {
    const out = new Array<number | null>(24).fill(null);
    const src = lens.moneyMode ? (yM.records as HistoryEconomicsRecord[]) : (yE.records as HistoryEnergyRecord[]);
    const exp = new Array<number>(24).fill(0);
    for (const r of src as unknown as Record<string, number>[]) {
      if (typeof r.ts !== 'number' || r.ts < yRange.from || r.ts >= yRange.to) continue;
      const h = clock.parts(r.ts).h;
      const v = lens.moneyMode ? (r.c ?? 0) : toKwh(r.ec ?? 0);
      out[h] = (out[h] ?? 0) + v;
      exp[h] += toKwh((r.es ?? 0) < 10 ? 0 : r.es ?? 0);
    }
    // House view: yesterday's HOUSE hour too (import + own solar), so the cap compares like with like.
    if (solar) {
      for (const r of yG.records as HistoryEnergyRecord[]) {
        if (typeof r.ts !== 'number' || r.ts < yRange.from || r.ts >= yRange.to) continue;
        const h = clock.parts(r.ts).h;
        const self = Math.max(0, toKwh((r.es ?? 0) < 10 ? 0 : r.es ?? 0) - exp[h]);
        if (self > 0) out[h] = (out[h] ?? 0) + self;
      }
    }
    return out;
  }, [yE.records, yM.records, yG.records, solar, lens.moneyMode, yRange, clock]);
  const hasYday = yday.some((v) => v != null && v > 0);
  const fmtVal = (v: number) => (lens.moneyMode ? lens.fmt(v) : `${nf(v, 2)} ${t('units.kWh')}`);

  const nowHour = clock.parts(Math.floor(Date.now() / 1000)).h;
  const plotW = W - PAD_L - PAD_R;
  // The scale follows the CLOSED hours; a running hour above them is clipped with ▲ and its value
  // (charts audit: one unclosed bar must not flatten the day).
  const closedBars = bars.filter((b) => b.hour !== clock.parts(Math.floor(Date.now() / 1000)).h);
  // Bar height = the house's hour (import + own solar); without generation that is the import.
  const houseOf = (b: { imp: number; gen: number; exp: number }): number => b.imp + selfOf(b);
  const maxImp = Math.max(0.001, ...(closedBars.some((b) => houseOf(b) > 0) ? closedBars : bars).map(houseOf), ...yday.map((v) => v ?? 0));
  const anySolar = solar && bars.some((b) => selfOf(b) > 0);
  const maxExp = Math.max(0, ...bars.map((b) => b.exp));
  const topBand = H * 0.62 - PAD_T;         // space above baseline (import)
  const botBand = H - PAD_B - (H * 0.62);   // space below baseline (export)
  // ONE scale for both directions: a kWh is as tall above the line as below it (INP-R01).
  const perKwh = Math.min(topBand / maxImp, maxExp > 0 ? botBand / maxExp : Infinity);
  const anyPartial = bars.some((b) => b.partial && b.hour !== clock.parts(Math.floor(Date.now() / 1000)).h);
  const baseY = PAD_T + topBand;
  const span = Math.max(1, bars.length);
  const slot = plotW / span;
  const bw = Math.min(26, slot * 0.6);
  const stride = HOUR_STRIDES.find((k) => slot * k >= 28) ?? 12;

  return (
    <Card>
      <Box sx={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', flexWrap: 'wrap', columnGap: 2, rowGap: 0.5, mb: 1 }}>
        <Typography sx={{ fontSize: 12, letterSpacing: '0.02em' }} color="text.secondary">
          <span>{t('cards.chartTitle')}</span>{lens.moneyMode ? ` · ${t('cards.chartTitleMoney')}` : ''}
          {/* the same hours as numbers (charts audit §6: a table view for every chart) */}
          {' · '}<Link component={RouterLink} to={historyHref({ point: unitId, win: 'today', metric: lens.moneyMode ? 'money' : 'energy', view: 'table' })} underline="hover" sx={{ fontSize: 12 }}>{t('common.asTable')}</Link>
        </Typography>
        {bars.length > 0 && (
          <ChartLegend items={[
            ...(anySolar
              ? [{ label: t('legend.fromGrid'), color: ds.primary }, { label: t('legend.fromSolar'), color: ds.solar }]
              : [{ label: t('legend.imported'), color: ds.primary }]),
            // Legend follows the data: export bars exist without generation too (a reversed CT,
            // a battery) — UX audit HOME-10/GEN-07.
            ...(maxExp > 0 ? [{ label: t('legend.exported'), color: ds.flowExport }] : []),
            ...(bars.some((b) => b.has && b.hour === clock.parts(Math.floor(Date.now() / 1000)).h) ? [{ label: t('legend.currentHour'), color: ds.primary, mark: 'hatch' as const }] : []),
            ...(hasYday ? [{ label: t('legend.yesterday'), color: ds.ink, mark: 'line' as const }] : []),
            ...(anyPartial ? [{ label: t('legend.partialHour'), color: ds.primary, mark: 'outline' as const }] : []),
          ]} />
        )}
      </Box>
      {bars.length === 0 ? (
        <Box sx={{ py: 4, textAlign: 'center', color: ds.inkMuted, fontSize: 13 }}>
          {/* «No data for today» only when the hub answered with none — not while loading or offline (L3P-13). */}
          {histError ? t('cards.chartNoLink') : histLoading ? t('common.loading') : t('cards.chartNoData')}
        </Box>
      ) : (
        // The svg is absolutely positioned so its pixel width never feeds back into the
        // container's intrinsic size (it must follow the card, not hold it open).
        <Box ref={measureRef} sx={{ position: 'relative', width: '100%', minWidth: 0, height: H, overflow: 'hidden',
          '& > svg': { display: 'block', position: 'absolute', left: 0, top: 0 } }}>
          <svg viewBox={`0 0 ${W} ${H}`} width={W} height={H} role="img" aria-label={t('cards.chartAria')}>
            <defs>
              <pattern id="oec-hatch" width="5" height="5" patternTransform="rotate(45)" patternUnits="userSpaceOnUse">
                <rect width="5" height="5" fill={ds.primary} opacity="0.25" />
                <line x1="0" y1="0" x2="0" y2="5" stroke={ds.primary} strokeWidth="2" />
              </pattern>
              <pattern id="oec-hatch-sun" width="5" height="5" patternTransform="rotate(45)" patternUnits="userSpaceOnUse">
                <rect width="5" height="5" fill={ds.solar} opacity="0.25" />
                <line x1="0" y1="0" x2="0" y2="5" stroke={ds.solar} strokeWidth="2" />
              </pattern>
            </defs>
            {/* baseline */}
            <line x1={PAD_L} x2={W - PAD_R} y1={baseY} y2={baseY} stroke={ds.borderStrong} strokeWidth="1" />
            {/* A scale: the tallest hour as a dashed guide with its value (H3P-23: no Y labels at all). */}
            <line x1={PAD_L} x2={W - PAD_R} y1={baseY - maxImp * perKwh} y2={baseY - maxImp * perKwh} stroke={ds.border} strokeWidth="1" strokeDasharray="3 4" />
            <text x={W - PAD_R} y={baseY - maxImp * perKwh - 4} textAnchor="end" fill={ds.inkMuted} fontSize="10">{fmtVal(maxImp)}</text>
            {bars.map((b, i) => {
              const cx = PAD_L + slot * i + slot / 2;
              const self = selfOf(b);
              const house = b.imp + self;
              const houseFull = house * perKwh;
              const clipped = houseFull > topBand + 0.5;
              // Two sections: the grid's at the bottom, the house's own solar on top (clipped together).
              const k = clipped ? topBand / houseFull : 1;
              const impH = b.imp * perKwh * k;
              const selfH = self * perKwh * k;
              const expH = b.exp * perKwh;
              const current = b.hour === nowHour;
              // Hatch = the running hour only; a PAST partial hour (outage, restart) is faded and says
              // so in its tooltip (INP-R04/HOME-R07: both were «Этот час (ещё идёт)»).
              const pastPartial = b.partial && !current;
              const impFill = current ? 'url(#oec-hatch)' : ds.primary;
              return (
                <g key={i}>
                  {b.imp > 0 && (
                    <rect x={cx - bw / 2} y={baseY - impH} width={bw} height={Math.max(1, impH)} rx="2"
                      fill={pastPartial ? 'transparent' : impFill} stroke={pastPartial ? ds.primary : 'none'} strokeWidth={pastPartial ? 1.5 : 0} opacity={current ? 1 : 0.9}>
                      <title>{t('cards.chartImportTip', { hour: b.hour, value: fmtVal(b.imp) })}{pastPartial ? ` · ${t('legend.partialHour')}` : ''}</title>
                    </rect>
                  )}
                  {self > 0 && (
                    <rect x={cx - bw / 2} y={baseY - impH - selfH} width={bw} height={Math.max(1, selfH)} rx="2"
                      fill={pastPartial ? 'transparent' : current ? 'url(#oec-hatch-sun)' : ds.solar} stroke={pastPartial ? ds.solar : 'none'} strokeWidth={pastPartial ? 1.5 : 0} opacity={current ? 1 : 0.9}>
                      <title>{t('cards.chartSolarTip', { hour: b.hour, value: fmtVal(self), house: fmtVal(house) })}</title>
                    </rect>
                  )}
                  {clipped && (
                    <text x={cx} y={baseY - impH - selfH + 10} textAnchor="middle" fill={ds.ink} fontSize="10" fontWeight="600">▲ {fmtVal(house)}</text>
                  )}
                  {b.exp > 0 && (
                    <rect x={cx - bw / 2} y={baseY} width={bw} height={Math.max(1, expH)} rx="2"
                      fill={ds.flowExport} opacity={current ? 1 : 0.9}>
                      <title>{t('cards.chartExportTip', { hour: b.hour, value: fmtVal(b.exp) })}</title>
                    </rect>
                  )}
                  {b.hour % stride === 0 && cx < W - PAD_R - 16 && (
                    <text x={cx} y={H - 6} textAnchor="middle" fill={ds.inkMuted} fontSize="11">{b.hour}</text>
                  )}
                </g>
              );
            })}
            {/* yesterday: a cap over each hour at its value */}
            {hasYday && yday.map((v, h) => {
              if (v == null || v <= 0) return null;
              const i = bars.findIndex((b) => b.hour === h);
              if (i < 0) return null;
              const cx = PAD_L + slot * i + slot / 2;
              const yv = baseY - Math.min(v * perKwh, topBand);
              return <line key={`y${h}`} x1={cx - bw / 2 - 4} x2={cx + bw / 2 + 4} y1={yv} y2={yv} stroke={ds.ink} strokeWidth={2} strokeLinecap="round" opacity={0.7} />;
            })}
            <HoverLayer
              ts={bars.map((b) => b.hour)}
              xOf={(h) => PAD_L + slot * bars.findIndex((b) => b.hour === h) + slot / 2}
              title={(h) => `${String(h).padStart(2, '0')}:00–${String((h + 1) % 24).padStart(2, '0')}:00`}
              rows={(h) => {
                const b = bars.find((x) => x.hour === h);
                const v = yday[h];
                const self = b ? selfOf(b) : 0;
                return [
                  ...(b && b.has && solar ? [{ label: t('legend.house'), value: fmtVal(b.imp + self), color: ds.ink }] : []),
                  ...(b && b.has ? [{ label: t(solar ? 'legend.fromGrid' : 'legend.imported'), value: fmtVal(b.imp), color: ds.primary }] : []),
                  ...(b && b.has && solar ? [{ label: t('legend.fromSolar'), value: fmtVal(self), color: ds.solar }] : []),
                  ...(b && b.has && solar && b.gen > 0 ? [{ label: t('legend.solarProduced'), value: fmtVal(b.gen), color: ds.solar, muted: true }] : []),
                  ...(b && b.exp > 0 ? [{ label: t('legend.exported'), value: fmtVal(b.exp), color: ds.flowExport }] : []),
                  ...(b && b.has && b.hour === nowHour ? [{ label: t('legend.currentHour'), value: '', muted: true }] : []),
                  ...(v != null && v > 0 ? [{ label: t('legend.yesterday'), value: fmtVal(v), color: ds.ink, muted: true }] : []),
                ];
              }}
              left={PAD_L} right={W - PAD_R} top={PAD_T} bottom={H - PAD_B}
              ink={ds.ink} muted={ds.inkMuted} surface={ds.surface} border={ds.border} ariaLabel={t('cards.chartAria')} />
            <text x={W - PAD_R} y={H - 6} textAnchor="end" fill={ds.inkMuted} fontSize="11">{t('units.h')}</text>
          </svg>
        </Box>
      )}
    </Card>
  );
};
