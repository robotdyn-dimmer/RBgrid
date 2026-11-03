/**
 * HistoryPage — «История» (spec §7). Scenario-driven analytics over /api/v1/history:
 * pick point(s) (each carries its series colour), period, metric and a chart/table view.
 * Dark-theme-first, ru-RU, tabular figures, «—» for missing. Inline-SVG trend (no chart.js):
 * diverging bars for energy/money (import·cost up, export·revenue down), a line for
 * power/voltage; recessive gridlines, mono ticks, partial buckets hatched. The table renders
 * data quality IN WORDS. CSV export carries a rough size estimate in its label.
 */
import React, { useCallback, useEffect, useMemo, useState } from 'react';
import { useSessionData } from '../hooks/useLocalStorage';
import { HelpTip } from '../components/common/HelpTip';
import {
  Box, Typography, Stack, Button, Chip, Alert, CircularProgress,
  ToggleButton, ToggleButtonGroup, MenuItem, TextField, Table, TableBody, TableCell, TableHead, TableRow, IconButton } from '@mui/material';
import DownloadIcon from '@mui/icons-material/Download';
import { Card, StatusChip } from '@rbgrid/design-system';
import { useDsTokens } from '../theme/useDsTokens';
import { dsFonts } from '../theme/dsTokens';
import { useCapabilities, useConfigShadow } from '../core';
import type { JsonObject } from '../core';
import type { UnitEntityGroup } from '../entities/model';
import { useHistory } from '../hooks/useHistory';
import { useTelemetryEntities } from '../hooks/useTelemetryEntities';
import { HistoryRecord, HistoryPeriod } from '../types/history';
import { hasQualityBit, decodeQualityBits } from '../core/quality/quality';
import { NoHistoryNote } from '../components/common/NoHistoryNote';
import { useUnitsConfig } from '../hooks/useUnitsConfig';
import { fullDisplayName, isGenerationRole, hexKey } from '../entities/siteModel';
import { enWh, pwW, volt, reactive, nf } from '../components/common/ruFormat';
import { useMoneyFormat } from '../hooks/useMoneyFormat';
import { useElementWidth } from '../hooks/useElementWidth';
import { intlLocale, t, dateLocale } from '../i18n';
import { useSearchParams } from 'react-router-dom';
import { parseHistoryLink } from './historyLink';
import { HIST_WINDOWS, HistWindow, windowSpanAt, windowTotals, deltaPct, Span } from './historyWindow';
import ChevronLeftIcon from '@mui/icons-material/ChevronLeft';
import ChevronRightIcon from '@mui/icons-material/ChevronRight';
import { useDeviceClock, formatDeviceTime, DeviceClock } from '../core/deviceTime';
import { buildHistoryCsv, buildSummaryCsv } from './historyCsv';
import { usePeriodTotals, PeriodTotals, TotalsPeriod } from '../hooks/usePeriodTotals';
import { useGridSystem } from '../hooks/useGridSystem';
import { ChartLegend } from '../components/common/ChartLegend';
import { linePath, bandPath, runsOf, Curve } from '../components/charts/chartPath';
import { HoverLayer } from '../components/charts/HoverLayer';
import { stepLabels } from '../components/charts/axisLabels';
import { unitMetricValue } from '../entities/model';
import { dayAvgRate } from '../economics/costRate';
import { getApiClient } from '../services/api';
import { isDemo } from '../services/connection';
import { demoHistory } from '../mock/demo';
import { toIntervalStarts, wireRange } from '../core/time/recordTime';
import { useHardware } from '../hooks/useHardware';
import { isBasic, pointHardware } from '../entities/moduleLine';

// ── Spec §7 switches ──────────────────────────────────────────────────────
type ViewMetric = 'energy' | 'power' | 'voltage' | 'reactive' | 'money';
type Kind = 'bars' | 'line';
interface MetricDef {
  value: ViewMetric;
  label: string;
  /** Which /api/v1/history family to fetch for this metric. */
  historyType: 'electrical' | 'energy' | 'economics';
  kind: Kind;
}
/** Type control: «Энергия» / «Мощность» / «Напряжение» / «Деньги» (§7.4). */
const METRICS: MetricDef[] = [
  { value: 'energy', label: 'history.metricEnergy', historyType: 'energy', kind: 'bars' },
  { value: 'power', label: 'history.metricPower', historyType: 'electrical', kind: 'line' },
  { value: 'voltage', label: 'history.metricVoltage', historyType: 'electrical', kind: 'line' },
  // Reactive power and PF per hour (3φ audit I3P-03): the sign says inductive (+) / capacitive (−).
  { value: 'reactive', label: 'history.metricReactive', historyType: 'electrical', kind: 'line' },
  { value: 'money', label: 'history.metricMoney', historyType: 'economics', kind: 'bars' },
];

const MAX_SERIES = 6; // ds.series carries 6 colours (§7.2)
const WALLCLOCK_FLOOR = 1.6e9; // ts below this = "before time sync" (uptime-style)

// ── small helpers ────────────────────────────────────────────────────────
const num = (r: HistoryRecord, key: string): number | null => {
  const v = (r as unknown as Record<string, unknown>)[key];
  return typeof v === 'number' ? v : null;
};
const isSynced = (ts: number): boolean => ts >= WALLCLOCK_FLOOR;


/** 1-2-5 «round» axis ticks covering [lo, hi]; 0 is a tick whenever the range crosses it. */
export function niceTicks(lo: number, hi: number, target = 4): number[] {
  if (!(hi > lo)) { hi = lo + 1; }
  const raw = (hi - lo) / target;
  const mag = Math.pow(10, Math.floor(Math.log10(raw)));
  const step = [1, 2, 2.5, 5, 10].map((k) => k * mag).find((s) => s >= raw) ?? 10 * mag;
  const a = Math.floor(lo / step) * step, b = Math.ceil(hi / step) * step;
  const out: number[] = [];
  for (let v = a; v <= b + step / 2; v += step) out.push(Math.abs(v) < step / 1e6 ? 0 : Number(v.toPrecision(12)));
  return out;
}

/** Nominal bucket length per step — gap detection on the charts. */
const STEP_SEC: Record<HistoryPeriod, number> = { hour: 3600, day: 86400, week: 7 * 86400, month: 30.44 * 86400, year: 365.25 * 86400 };

/** "23 ч 40 мин" / "24 ч" / "45 мин" / "30 с". */
function formatDur(sec: number): string {
  if (!Number.isFinite(sec) || sec <= 0) return '—';
  const h = Math.floor(sec / 3600);
  const m = Math.round((sec % 3600) / 60);
  if (h && m) return t('history.durHM', { h, m });
  if (h) return t('history.durH', { h });
  if (m) return t('history.durM', { m });
  return t('history.durS', { s: Math.round(sec) });
}

/** Time label for a bucket, per period. */
function formatTs(ts: number, period: HistoryPeriod, clock: DeviceClock): string {
  if (!isSynced(ts)) return t('history.beforeSync');
  const f = (o: Intl.DateTimeFormatOptions) => formatDeviceTime(clock, ts, o, dateLocale());
  if (period === 'hour') return f({ hour: '2-digit', minute: '2-digit' });
  // Months by name on «This year» (charts audit O15: «04.01, 01.02» read as dates).
  if (period === 'year' || period === 'month') return f({ month: 'short' });
  return f({ day: '2-digit', month: '2-digit' });
}

/** Full cell for the «Время» column. */
function formatRowTime(ts: number, period: HistoryPeriod, clock: DeviceClock, dur?: number | null): string {
  if (!isSynced(ts)) return t('history.beforeSync');
  const f = (o: Intl.DateTimeFormatOptions) => formatDeviceTime(clock, ts, o, dateLocale());
  // An hour row reads as its interval «16:00–17:00» — «17:02» alone left open whether it is the
  // start or the end (3φ audit H3P-14).
  if (period === 'hour') {
    const end = formatDeviceTime(clock, ts + (dur && dur > 0 ? dur : 3600), { hour: '2-digit', minute: '2-digit' }, dateLocale());
    return `${f({ hour: '2-digit', minute: '2-digit' })}–${end}`;
  }
  return f({ day: '2-digit', month: 'short', year: 'numeric' });
}

/**
 * Full length of the record's bucket, from the PERIOD (not from the data): hour 3600 s, day
 * 86 400 s, week 7 d, month/year by the calendar of the bucket's start. UX audit RES-01: the
 * nominal used to be max(dur) over the data, so one anomalous 4500 s hour marked every complete
 * hour on .110 «частично 1 ч из 1 ч 15 мин».
 */
export function bucketNominal(ts: number, period: HistoryPeriod, clock?: DeviceClock): number {
  if (period === 'hour') return 3600;
  if (period === 'day') return 86400;
  if (period === 'week') return 7 * 86400;
  const d = new Date(ts * 1000);
  // The month / year of the bucket's start by the hub's calendar when known.
  const p = clock ? clock.parts(ts) : { y: d.getFullYear(), mo: d.getMonth() + 1 };
  if (period === 'month') return new Date(Date.UTC(p.y, p.mo, 0)).getUTCDate() * 86400;
  const y = p.y;
  return ((y % 4 === 0 && y % 100 !== 0) || y % 400 === 0 ? 366 : 365) * 86400;
}

/** A record is partial when its covered duration is below its bucket's nominal. */
export const isPartial = (r: HistoryRecord, period: HistoryPeriod, clock?: DeviceClock): boolean => {
  const dur = num(r, 'dur');
  return dur != null && dur < bucketNominal(r.ts, period, clock) * 0.999;
};

/** The v4 quality mask: `q` is quality only on records that carry the `dur` header (on a v3
 *  ELECTRICAL record the same key was reactive power — canon fa34d5d3 renamed it to p_rea). */
const q4 = (r: HistoryRecord): number => (num(r, 'dur') != null ? num(r, 'q') ?? 0 : 0);

/**
 * Quality IN WORDS (§7.5, UX audit RES-17): «полный», «неполный: 20 мин из 1 ч»,
 * «длиннее нормы: 1 ч 15 мин», plus the set bits as phrases («время не синхронизировано»,
 * «перенесено из старого формата»…), never raw bit names.
 */
export function qualityWord(r: HistoryRecord, period: HistoryPeriod, clock?: DeviceClock, skipBits: number[] = []): string {
  const dur = num(r, 'dur');
  const q = q4(r);
  const nominal = bucketNominal(r.ts, period, clock);
  const parts: string[] = [];
  if (dur != null && dur > nominal * 1.001) parts.push(t('history.qualityLong', { dur: formatDur(dur) }));
  else if (isPartial(r, period, clock)) parts.push(t('history.qualityCoverage', { word: t('history.qualityPartial'), covered: formatDur(dur ?? 0), total: formatDur(nominal) }));
  for (const bit of decodeBitsIdx(q)) {
    if (bit === 0 || skipBits.includes(bit)) continue; // PARTIAL — already said with the coverage
    parts.push(tBit(bit));
  }
  // The hub's day total was short of its own hours — taken from the hours (RES-R01).
  if ((r as { reconciled?: boolean }).reconciled) parts.push(t('history.reconciled'));
  return parts.length ? parts.join(', ') : t('history.qualityFull');
}

const decodeBitsIdx = (q: number): number[] => Array.from({ length: 16 }, (_, b) => b).filter((b) => hasQualityBit(q, b));
const tBit = (bit: number): string => {
  const s = t(`qbit.${bit}`);
  return s === `qbit.${bit}` ? (decodeQualityBits(1 << bit)[0] ?? `bit ${bit}`) : s;
};

// ── hidden per-point fetcher (hooks can't loop, so one <PointFetcher> per point) ──
interface PointResult { records: HistoryRecord[]; loading: boolean; error: string | null }
const PointFetcher: React.FC<{
  unitKey: string;
  period: HistoryPeriod;
  historyType: 'electrical' | 'energy' | 'economics';
  range: Span;
  limit: number;
  onResult: (key: string, r: PointResult) => void;
}> = ({ unitKey, period, historyType, range, limit, onResult }) => {
  const { records, loading, error } = useHistory(unitKey, period, historyType, limit, range);
  useEffect(() => {
    onResult(unitKey, { records, loading, error });
  }, [unitKey, records, loading, error, onResult]);
  return null;
};

// ── inline-SVG trend (no chart.js) ─────────────────────────────────────────
interface SeriesData { key: string; color: string; records: HistoryRecord[] }
// Fixed 320 px height; the width is the measured card width, drawn in REAL pixels
// (`viewBox = 0 0 W H`) so the 11 px mono ticks never scale (review 03 P0-8).
const CHART_H = 320, M = { t: 14, r: 16, b: 30, l: 62 };
const H = CHART_H;
const PH = H - M.t - M.b;
/** Min px between x tick labels («ЧЧ:ММ» / «ДД.ММ» at 11 px mono ≈ 34 px wide). */
const X_TICK_GAP = 64;
/** Every k-th of `n` ticks so they keep ≥ X_TICK_GAP px apart over `pw` px. */
/** Left margin that fits the widest y label (11 px mono ≈ 6.7 px/glyph) + 10 px gap. */
const leftMargin = (ticks: number[], fmt: (v: number) => string): number =>
  Math.max(M.l, Math.ceil(Math.max(...ticks.map((t) => Array.from(fmt(t)).length)) * 6.7) + 10);
const tickStride = (n: number, pw: number): number => Math.max(1, Math.ceil(n / Math.max(2, Math.floor(pw / X_TICK_GAP))));

const SvgTrend: React.FC<{
  series: SeriesData[];
  kind: Kind;
  period: HistoryPeriod;
  /** line value, already in display units (W / V). */
  valueOf: (r: HistoryRecord) => number | null;
  /** diverging-bar up/down values, already in display units. */
  upOf?: (r: HistoryRecord) => number | null;
  downOf?: (r: HistoryRecord) => number | null;
  /** min–max band around the line (voltage dips / surges — UX audit RES-15). */
  bandOf?: (r: HistoryRecord) => [number, number] | null;
  fmtY: (v: number) => string;
  /** Line shape (charts audit 17.10): hour means as a step, day/month trends and voltage smooth. */
  curve?: Curve;
  /** Voltage: the axis is never narrower than ±5 % of this nominal and shows the ±10 % limits. */
  nominalV?: number;
  /** A day window (Today / Yesterday): the axis spans the whole day, 0–24 h (charts audit O6). */
  xRange?: { from: number; to: number };
  /** The input's series key: its export goes down in the export colour; other points keep theirs. */
  inputKey?: string;
  /** «Total and parts»: the input as one bar of the selected lines + «the rest» (audit B1). */
  stacked?: boolean;
  nameOf?: (key: string) => string;
  /** The previous period of one point, shifted by `offset` s onto this period's axis. */
  ghost?: { key: string; records: HistoryRecord[]; offset: number };
  /** Chart height, px (a companion strip — cos φ under Q — is shorter). */
  height?: number;
  /** A fixed value window the axis always holds (cos φ: 0.5–1.0), widened by the data. */
  yWindow?: [number, number];
  /** The previous period's name in the tooltip («Yesterday»). */
  ghostLabel?: string;
}> = ({ series, kind, period, valueOf, upOf, downOf, bandOf, fmtY, curve = 'monotoneX', nominalV, xRange, inputKey, stacked, nameOf, ghost, height: heightProp = H, yWindow, ghostLabel }) => {
  const [measureRefEarly, measuredEarly] = useElementWidth<HTMLDivElement>(820);
  // Phones: a 220 px chart keeps the legend and the chart on one screen (charts spec 17.10).
  const height = heightProp === CHART_H && measuredEarly < 600 ? 220 : heightProp;
  const PH = height - M.t - M.b;
  const ds = useDsTokens();
  const clock = useDeviceClock();
  const measureRef = measureRefEarly, measured = measuredEarly;
  const W = Math.max(280, measured);

  // The previous period moved onto this period's axis (day N ↔ day N, hour h ↔ hour h).
  const ghostRecs = useMemo(() => (ghost
    ? ghost.records.filter((r) => isSynced(r.ts)).map((r) => ({ ...r, ts: r.ts + ghost.offset })).sort((a, b) => a.ts - b.ts)
    : []), [ghost]);
  const ghostColor = ghost ? series.find((x) => x.key === ghost.key)?.color : undefined;
  // synced records per series, ascending in time
  const synced = useMemo(
    () => series.map((s) => ({
      ...s,
      recs: s.records.filter((r) => isSynced(r.ts)).sort((a, b) => a.ts - b.ts),
    })),
    [series]
  );

  if (kind === 'line') {
    const all = synced.flatMap((s) => s.recs);
    const tsAll = [...all.map((r) => r.ts), ...ghostRecs.map((r) => r.ts)];
    const vals = [...all, ...ghostRecs].flatMap((r) => [valueOf(r), ...(bandOf?.(r) ?? [])]).filter((v): v is number => v != null);
    if (!all.length || !vals.length) return null;
    const stepSec = STEP_SEC[period];
    const tMin = xRange ? Math.min(xRange.from, ...tsAll) : Math.min(...tsAll);
    // A step holds the last value over its own interval: the axis runs to that interval's end.
    const tMax = xRange ? Math.max(xRange.to, ...tsAll) : Math.max(...tsAll) + (curve === 'step' ? stepSec : 0);
    let yMin = Math.min(...vals), yMax = Math.max(...vals);
    // Voltage: the window always holds the ±10 % limits — 226–234 V drawn full height read as an
    // emergency (charts audit L2); the limit lines say «normal» without reading numbers.
    if (nominalV) { yMin = Math.min(yMin, nominalV * 0.88); yMax = Math.max(yMax, nominalV * 1.12); }
    if (yWindow) { yMin = Math.min(yMin, yWindow[0]); yMax = Math.max(yMax, yWindow[1]); }
    if (yMin === yMax) { yMin -= 1; yMax += 1; }
    const nonNeg = yMin >= 0;
    const pad = nominalV || yWindow ? 0 : (yMax - yMin) * 0.08; yMin -= pad; yMax += pad;
    // No negative values → the axis doesn't dip below 0 («−500 W» without export, H3P-14/I3P-22).
    if (nonNeg && !nominalV && !yWindow) yMin = Math.max(0, yMin);
    const nt = niceTicks(yMin, yMax); yMin = nt[0]; yMax = nt[nt.length - 1];
    const yTicks = nt;
    const ML = leftMargin(yTicks, fmtY), PW = W - ML - M.r;
    const xFor = (ts: number) => ML + ((ts - tMin) / (tMax - tMin || 1)) * PW;
    const yFor = (v: number) => M.t + (1 - (v - yMin) / (yMax - yMin || 1)) * PH;
    // Day window: ticks every 3 h over the whole day, not only the hours with data.
    const xTs = xRange && period === 'hour'
      ? Array.from({ length: Math.floor((xRange.to - xRange.from) / 3600) + 1 }, (_, i) => xRange.from + i * 3600)
      // a fragment of an hour (10:40 after a restart) gets no label of its own — «10:4011:00» (audit)
      : Array.from(new Set(tsAll)).sort((a, b) => a - b).filter((x) => period !== 'hour' || clock.parts(x).mi === 0);
    const xStride = tickStride(xTs.length, PW);
    const xTickTs = xTs.filter((_, i) => i % xStride === 0);
    // The voltage band of several points piled into one brown block (L4): only with one point.
    const showBand = !!bandOf && synced.filter((s) => s.recs.length).length === 1;
    const limits = nominalV ? [nominalV * 0.9, nominalV * 1.1].filter((v) => v >= yMin && v <= yMax) : [];

    return (
      <SvgFrame ds={ds} W={W} H={height} ML={ML} measureRef={measureRef} yTicks={yTicks} yFor={yFor} fmtY={fmtY}
        xTickTs={xTickTs} xFor={xFor} period={period}>
        {limits.map((v) => (
          <g key={`lim${v}`}>
            <line x1={ML} x2={ML + PW} y1={yFor(v)} y2={yFor(v)} stroke={ds.sevWarning} strokeWidth={1} strokeDasharray="6 4" opacity={0.7} />
            <text x={ML + PW - 2} y={yFor(v) - 3} textAnchor="end" fontSize={10} fill={ds.inkMuted}>{v > (nominalV ?? 0) ? '+10 %' : '−10 %'}</text>
          </g>
        ))}
        {/* the previous period: the same colour, thinner and quieter, under the current line */}
        {ghostColor && runsOf(ghostRecs, (r) => r.ts, valueOf, stepSec * 1.5).map((rs, gi) => (
          <path key={`g${gi}`} d={linePath(rs.map((r) => ({ x: xFor(r.ts), y: yFor(valueOf(r) as number) })), curve, xFor(rs[rs.length - 1].ts + stepSec))}
            fill="none" stroke={ghostColor} strokeWidth={1.5} opacity={0.45} strokeLinejoin="round" strokeLinecap="round" />
        ))}
        {synced.map((s) => {
          // split into runs on gaps: a missing bucket is a hole, not a straight line across
          // hours without data (UX audit RES-16: a 4-h outage on .110 looked like a ramp)
          const gap = stepSec * 1.5;
          const runs = runsOf(s.recs, (r) => r.ts, valueOf, gap);
          const pts = (rs: HistoryRecord[], v: (r: HistoryRecord) => number) => rs.map((r) => ({ x: xFor(r.ts), y: yFor(v(r)) }));
          const endOf = (rs: HistoryRecord[]) => xFor(rs[rs.length - 1].ts + stepSec);
          return (
            <g key={s.key}>
              {showBand && bandOf && runsOf(s.recs, (r) => r.ts, (r) => (bandOf(r) ? 1 : null), gap).filter((rs) => rs.length > 1).map((rs, bi) => (
                <path key={`b${bi}`} d={bandPath(pts(rs, (r) => (bandOf(r) as [number, number])[0]), pts(rs, (r) => (bandOf(r) as [number, number])[1]), curve, endOf(rs))}
                  fill={s.color} opacity={0.1} stroke="none" />
              ))}
              {runs.map((rs, ri) => (
                <path key={`l${ri}`} d={linePath(pts(rs, (r) => valueOf(r) as number), curve, endOf(rs))} fill="none" stroke={s.color} strokeWidth={2}
                  strokeLinejoin="round" strokeLinecap="round" />
              ))}
              {/* a lone reading is a dot, not an invisible move (charts audit O4) */}
              {runs.filter((rs) => rs.length === 1 && curve !== 'step').map((rs) => (
                <circle key={`o${rs[0].ts}`} cx={xFor(rs[0].ts)} cy={yFor(valueOf(rs[0]) as number)} r={4} fill={s.color} stroke={ds.bg} strokeWidth={2} />
              ))}
              {/* hover targets with the exact value (RES-07: no way to read a point) */}
              {s.recs.map((r) => {
                const v = valueOf(r);
                if (v == null) return null;
                const b = bandOf?.(r);
                const x = curve === 'step' ? xFor(r.ts + stepSec / 2) : xFor(r.ts);
                return (
                  <circle key={`h${r.ts}`} cx={x} cy={yFor(v)} r={7} fill="transparent">
                    <title>{`${formatRowTime(r.ts, period, clock, num(r, 'dur'))}: ${fmtY(v)}${b ? ` (${fmtY(b[0])} – ${fmtY(b[1])})` : ''}`}</title>
                  </circle>
                );
              })}
              {/* an incomplete interval: a hollow ring in the series colour, not an alarm colour */}
              {s.recs.map((r) => {
                const v = valueOf(r);
                if (v == null || !isPartial(r, period, clock)) return null;
                const x = curve === 'step' ? xFor(r.ts + stepSec / 2) : xFor(r.ts);
                return <circle key={r.ts} cx={x} cy={yFor(v)} r={3.5} fill={ds.bg} stroke={s.color} strokeWidth={1.5} />;
              })}
            </g>
          );
        })}
        <HoverLayer
          ts={Array.from(new Set(all.map((r) => r.ts))).sort((a, b) => a - b)}
          xOf={(ts) => (curve === 'step' ? xFor(ts + stepSec / 2) : xFor(ts))}
          title={(ts) => formatRowTime(ts, period, clock)}
          rows={(ts) => [
            ...synced.flatMap((sr) => {
              const r = sr.recs.find((q) => q.ts === ts);
              const v = r ? valueOf(r) : null;
              const b = r ? bandOf?.(r) : null;
              return v == null ? [] : [{ label: nameOf?.(sr.key) ?? '', value: `${fmtY(v)}${b ? ` (${fmtY(b[0])}–${fmtY(b[1])})` : ''}`, color: sr.color }];
            }),
            ...ghostRecs.filter((q) => Math.abs(q.ts - ts) < stepSec / 2).flatMap((q) => {
              const v = valueOf(q);
              return v == null ? [] : [{ label: ghostLabel ?? '', value: fmtY(v), color: ghostColor, muted: true }];
            }),
          ]}
          left={ML} right={ML + PW} top={M.t} bottom={M.t + PH}
          ink={ds.ink} muted={ds.inkMuted} surface={ds.surface} border={ds.border} ariaLabel={t('history.chartAria')} />
      </SvgFrame>
    );
  }

  // diverging bars (energy / money)
  const up = upOf ?? valueOf, down = downOf ?? (() => null);
  const bucketTs = Array.from(new Set(synced.flatMap((s) => s.recs.map((r) => r.ts)))).sort((a, b) => a - b);
  if (!bucketTs.length) return null;
  let maxUp = 0, maxDown = 0;
  for (const r of [...synced.flatMap((s) => s.recs), ...ghostRecs]) {
    const u = up(r); const dn = down(r);
    if (u != null) maxUp = Math.max(maxUp, u);
    if (dn != null) maxDown = Math.max(maxDown, dn);
  }
  if (maxUp === 0 && maxDown === 0) maxUp = 1;
  // Metering crumbs on the other side (a few Wh of «export») don't open a «−0.2 kWh» half (H3P-14).
  if (maxDown < maxUp * 0.02) maxDown = 0;
  // «Round» ticks (1-2-5) with 0 always one of them (RES-23: «0,1 / −0,4 кВт·ч», no zero line tick).
  const yTicks = niceTicks(maxDown > 0 ? -maxDown : 0, maxUp);
  const yMax = yTicks[yTicks.length - 1], yMin = yTicks[0];
  const ML = leftMargin(yTicks, fmtY), PW = W - ML - M.r;
  const yFor = (v: number) => M.t + (1 - (v - yMin) / (yMax - yMin || 1)) * PH;
  const zeroY = yFor(0);
  // Time-true slots: a bucket without data stays an empty slot (RES-16), not squeezed out.
  const step = STEP_SEC[period];
  const t0 = xRange ? Math.min(xRange.from, bucketTs[0]) : bucketTs[0];
  const slotOf = (ts: number) => Math.round((ts - t0) / step);
  const lastGhost = ghostRecs.length ? ghostRecs[ghostRecs.length - 1].ts : bucketTs[0];
  const n = Math.min(400, Math.max(slotOf(bucketTs[bucketTs.length - 1]) + 1, slotOf(lastGhost) + 1, xRange ? Math.round((xRange.to - t0) / step) : 0));
  const slotW = PW / n;
  const nS = Math.max(1, synced.length);
  // Bars ≤ 24 px, 2 px gaps inside a group, the group ≤ 60 % of its slot (charts spec 17.10).
  const gap = nS > 1 && slotW > 12 * nS ? 2 : 0;
  const barW = Math.max(1.5, Math.min(24, (slotW * 0.6 - gap * (nS - 1)) / nS));
  const groupW = barW * nS + gap * (nS - 1);
  const tsIndex = new Map(bucketTs.map((t) => [t, Math.min(n - 1, slotOf(t))]));
  // one label per slot (16:00 and 16:17 after a reboot share the hour slot)
  // Ticks over every slot when the axis runs past the data (a whole day, or the previous period's tail).
  const slotFirst = (xRange && period === 'hour') || (ghostRecs.length > 0 && period !== 'month' && slotOf(lastGhost) > slotOf(bucketTs[bucketTs.length - 1]))
    ? Array.from({ length: n }, (_, i) => t0 + i * step)
    : bucketTs.filter((t, i) => (i === 0 || slotOf(t) !== slotOf(bucketTs[i - 1])) && (period !== 'hour' || clock.parts(t).mi === 0));
  const xStride = tickStride(slotFirst.length, PW);
  const xTickTs = slotFirst.filter((_, i) => i % xStride === 0);
  const slotCenter = (i: number) => ML + (i + 0.5) * slotW;
  // «Total and parts» plan: per slot, the input's up value split into the selected lines + the rest.
  const inputS = stacked && inputKey ? synced.find((x) => x.key === inputKey) : undefined;
  const stackW = Math.max(2, Math.min(24, slotW * 0.6));
  const stackPlan = inputS && synced.length > 1 ? inputS.recs.flatMap((r) => {
    const idx = tsIndex.get(r.ts);
    const total = up(r);
    if (idx == null || total == null) return [];
    const parts = synced.filter((x) => x !== inputS).map((x) => {
      const rr = x.recs.find((q) => Math.abs(q.ts - r.ts) < step / 2);
      return { name: nameOf?.(x.key) ?? x.key, color: x.color, v: Math.max(0, (rr && up(rr)) || 0), rest: false };
    }).filter((pt) => pt.v > 0);
    const sum = parts.reduce((a, pt) => a + pt.v, 0);
    const rest = Math.max(0, total - sum);
    if (rest > 0) parts.push({ name: t('history.restOfInput'), color: ds.inkMuted, v: rest, rest: true });
    return [{ ts: r.ts, idx, total: Math.max(total, sum), parts, down: Math.max(0, down(r) ?? 0), partial: isPartial(r, period, clock) }];
  }) : null;

  return (
    <SvgFrame ds={ds} W={W} H={height} ML={ML} measureRef={measureRef} yTicks={yTicks} yFor={yFor} fmtY={fmtY}
      xTickTs={xTickTs} xFor={(ts) => slotCenter(tsIndex.get(ts) ?? Math.min(n - 1, Math.max(0, slotOf(ts))))} period={period}>
      {/* emphasised zero baseline */}
      <line x1={ML} x2={ML + PW} y1={zeroY} y2={zeroY} stroke={ds.inkMuted} strokeWidth={1} opacity={0.5} />
      {stackPlan ? (
        // «Total and parts»: one bar per slot — the selected lines stacked in their colours, the
        // input's remainder grey on top, the input's total above it; export down from zero.
        stackPlan.map((c) => {
          const cx = slotCenter(c.idx) - stackW / 2;
          let acc = 0;
          const segs = c.parts.map((pt) => {
            const y0 = yFor(acc), y1 = yFor(acc + pt.v);
            acc += pt.v;
            return { ...pt, y: y1, h: y0 - y1 };
          });
          const top = segs.length ? segs[segs.length - 1].y : zeroY;
          return (
            <g key={c.ts}>
              <title>{`${formatRowTime(c.ts, period, clock)}: ${fmtY(c.total)}${c.parts.map((pt) => `\n${pt.name}: ${fmtY(pt.v)}`).join('')}${c.down > 0 ? `\n↓ ${fmtY(c.down)}` : ''}`}</title>
              {segs.map((sg, k) => (
                // 2 px of surface between sections; only the top one rounded
                k === segs.length - 1
                  ? <BarRect key={k} x={cx} y={sg.y} w={stackW} h={Math.max(0, sg.h - (k ? 2 : 0))} color={sg.color} hatched={c.partial} opacity={sg.rest ? 0.35 : 0.9} />
                  : <rect key={k} x={cx} y={sg.y + (k ? 2 : 0)} width={stackW} height={Math.max(0, sg.h - (k ? 2 : 0))} fill={sg.color} opacity={sg.rest ? 0.35 : 0.9} />
              ))}
              {c.down > 0 && <BarRect x={cx} y={zeroY} w={stackW} h={yFor(-c.down) - zeroY} color={ds.flowExport} hatched={c.partial} down />}
              {slotW >= 7 * fmtY(c.total).length && <text x={cx + stackW / 2} y={top - 4} textAnchor="middle" fontSize={10} fill={ds.inkMuted}>{fmtY(c.total)}</text>}
            </g>
          );
        })
      ) : synced.map((s, si) => (
        <g key={s.key}>
          {s.recs.map((r) => {
            const idx = tsIndex.get(r.ts);
            if (idx == null) return null;
            const cx = slotCenter(idx) - groupW / 2 + si * (barW + gap);
            const u = up(r), dn = down(r);
            const part = isPartial(r, period, clock);
            const bars: React.ReactNode[] = [];
            if (u != null && u > 0) {
              const y = yFor(u);
              bars.push(<BarRect key="u" x={cx} y={y} w={barW} h={zeroY - y} color={s.color} hatched={part} />);
            }
            if (dn != null && dn > 0) {
              const y2 = yFor(-dn);
              // Down from zero: the input's export in the export colour (as on the Overview); any
              // other point (a PV producing) keeps its own colour — paleness meant «partial».
              bars.push(<BarRect key="d" x={cx} y={zeroY} w={barW} h={y2 - zeroY} color={s.key === inputKey ? ds.flowExport : s.color} hatched={part} down />);
            }
            return (
              <g key={r.ts}>
                <title>{`${formatRowTime(r.ts, period, clock, num(r, 'dur'))}: ${u != null ? fmtY(u) : '—'}${dn != null && dn > 0 ? ` · ↓ ${fmtY(dn)}` : ''}`}</title>
                {bars}
              </g>
            );
          })}
        </g>
      ))}
      {/* the previous period: a cap over each slot at its value (the bars keep their size) */}
      {ghostColor && ghostRecs.map((r) => {
        const i = slotOf(r.ts);
        if (i < 0 || i >= n) return null;
        // side by side with several points: the cap sits over its own point's bar only
        const gi = synced.findIndex((x) => x.key === ghost?.key);
        const own = !stackPlan && synced.length > 1 && gi >= 0;
        const wCap = own ? barW + 6 : (stackPlan ? stackW : groupW) + 8;
        const x0 = own ? slotCenter(i) - groupW / 2 + gi * (barW + gap) - 3 : slotCenter(i) - wCap / 2;
        const u = up(r), dn = down(r);
        return (
          <g key={`g${r.ts}`} opacity={0.75}>
            {u != null && u > 0 && <line x1={x0} x2={x0 + wCap} y1={yFor(u)} y2={yFor(u)} stroke={ds.ink} strokeWidth={2} strokeLinecap="round" />}
            {dn != null && dn > 0 && <line x1={x0} x2={x0 + wCap} y1={yFor(-dn)} y2={yFor(-dn)} stroke={ds.ink} strokeWidth={2} strokeLinecap="round" />}
          </g>
        );
      })}
      <HoverLayer
        ts={bucketTs}
        xOf={(ts) => slotCenter(Math.min(n - 1, Math.max(0, slotOf(ts))))}
        title={(ts) => formatRowTime(ts, period, clock)}
        rows={(ts) => {
          const c = stackPlan?.find((x) => x.ts === ts);
          const rowsOut = c
            ? [{ label: nameOf?.(inputKey ?? '') ?? '', value: fmtY(c.total) }, ...c.parts.map((pt) => ({ label: pt.name, value: fmtY(pt.v), color: pt.color, muted: pt.rest })), ...(c.down > 0 ? [{ label: t('legend.exportedDown'), value: fmtY(c.down), color: ds.flowExport }] : [])]
            : synced.flatMap((sr) => {
              const r = sr.recs.find((q) => q.ts === ts);
              if (!r) return [];
              const u = up(r), dn = down(r);
              return [
                ...(u != null ? [{ label: nameOf?.(sr.key) ?? '', value: fmtY(u), color: sr.color }] : []),
                ...(dn != null && dn > 0 ? [{ label: `${nameOf?.(sr.key) ?? ''} ↓`, value: fmtY(dn), color: sr.key === inputKey ? ds.flowExport : sr.color }] : []),
              ];
            });
          const g = ghostRecs.find((q) => slotOf(q.ts) === slotOf(ts));
          const gu = g ? up(g) : null;
          return [...rowsOut, ...(gu != null ? [{ label: ghostLabel ?? '', value: fmtY(gu), color: ds.ink, muted: true }] : [])];
        }}
        left={ML} right={ML + PW} top={M.t} bottom={M.t + PH}
        ink={ds.ink} muted={ds.inkMuted} surface={ds.surface} border={ds.border} ariaLabel={t('history.chartAria')} />
    </SvgFrame>
  );
};

/** One bar; `dim` lightens the export/revenue (downward) side, `hatched` marks partial buckets. */
/** A bar rounded only at its far end from zero (charts spec 17.10); `down` = below the zero line. */
const BarRect: React.FC<{ x: number; y: number; w: number; h: number; color: string; hatched?: boolean; down?: boolean; opacity?: number }> =
  ({ x, y, w, h, color, hatched, down, opacity = 0.9 }) => {
    const hh = Math.max(0, h);
    const r = Math.min(3, w / 2, hh);
    // far end: top for an up bar, bottom for a down bar
    const d = down
      ? `M${x} ${y} H${x + w} V${y + hh - r} Q${x + w} ${y + hh} ${x + w - r} ${y + hh} H${x + r} Q${x} ${y + hh} ${x} ${y + hh - r} Z`
      : `M${x} ${y + hh} V${y + r} Q${x} ${y} ${x + r} ${y} H${x + w - r} Q${x + w} ${y} ${x + w} ${y + r} V${y + hh} Z`;
    return (
      <>
        <path d={d} fill={color} opacity={opacity} />
        {hatched && <path d={d} fill="url(#hist-hatch)" opacity={0.9} />}
      </>
    );
  };

/** Shared axis frame: recessive gridlines (ds.chartGrid), mono ticks (dsFonts.mono). */
const SvgFrame: React.FC<{
  ds: ReturnType<typeof useDsTokens>;
  /** Real pixel width (measured). */
  W: number;
  /** Left margin (fits the y labels). */
  ML: number;
  measureRef: (node: HTMLDivElement | null) => void;
  yTicks: number[];
  yFor: (v: number) => number;
  fmtY: (v: number) => string;
  xTickTs: number[];
  xFor: (ts: number) => number;
  period: HistoryPeriod;
  children: React.ReactNode;
  H?: number;
}> = ({ ds, W, ML, measureRef, yTicks, yFor, fmtY, xTickTs, xFor, period, children, H = CHART_H }) => {
  const tickLabels = stepLabels(yTicks, fmtY, intlLocale());
  const PW = W - ML - M.r;
  const clock = useDeviceClock();
  return (
  // svg absolutely positioned: its pixel width must not hold the card open when it shrinks.
  <Box ref={measureRef} sx={{ position: 'relative', width: '100%', minWidth: 0, height: H, overflow: 'hidden',
    '& > svg': { display: 'block', position: 'absolute', left: 0, top: 0 } }}>
    <svg viewBox={`0 0 ${W} ${H}`} width={W} height={H} role="img" aria-label={t('history.chartAria')}>
      <defs>
        <pattern id="hist-hatch" patternUnits="userSpaceOnUse" width={5} height={5}
          patternTransform="rotate(45)">
          <line x1={0} y1={0} x2={0} y2={5} stroke={ds.inkMuted} strokeWidth={1} />
        </pattern>
      </defs>
      {yTicks.map((v, i) => {
        const y = yFor(v);
        const label = tickLabels[i];
        return (
          <g key={i}>
            <line x1={ML} x2={ML + PW} y1={y} y2={y} stroke={ds.chartGrid} strokeWidth={1} />
            <text x={ML - 8} y={y + 3} textAnchor="end"
              fontFamily={dsFonts.mono} fontSize={11} fill={ds.inkMuted}>{label}</text>
          </g>
        );
      })}
      {xTickTs.map((ts, i) => {
        const x = xFor(ts);
        // keep edge labels inside the plot instead of clipping them at the svg border
        const anchor = x - 18 < 0 ? 'start' : x + 18 > W ? 'end' : 'middle';
        return (
          <text key={i} x={x} y={H - 10} textAnchor={anchor}
            fontFamily={dsFonts.mono} fontSize={11} fill={ds.inkMuted}>{formatTs(ts, period, clock)}</text>
        );
      })}
      {children}
    </svg>
  </Box>
  );
};

/** Toggle group as a segment row: full width + sideways scroll on phones (< sm 420). */
const segmentSx = {
  maxWidth: '100%',
  overflowX: 'auto',
  width: { xs: '100%', sm: 'auto' },
  '& .MuiToggleButton-root': { flex: { xs: '1 0 auto', sm: '0 0 auto' }, whiteSpace: 'nowrap' },
} as const;

// ── page ───────────────────────────────────────────────────────────────────
/** One point's running-period totals (usePeriodTotals) with today from its live counters. */
const TotalsProbe: React.FC<{ k: string; id: string; group?: UnitEntityGroup; period: TotalsPeriod; isInput: boolean;
  sys?: { e_day_in?: number; e_day_out?: number; cost_day?: number; rev_day?: number } | null; onTotals: (k: string, v: PeriodTotals | null) => void;
  onToday?: (k: string, v: TodaySoFar | null) => void }> = ({ k, id, group, period, isInput, sys, onTotals, onToday }) => {
  // Today: the input = the hub's day counters with money; a line = its own counters (energy only;
  // its money follows the input's average price of the day).
  const inWh = Math.round((isInput ? sys?.e_day_in : group ? unitMetricValue(group, 'energy_day_in') : null) ?? -1);
  const outWh = Math.round((isInput ? sys?.e_day_out : group ? unitMetricValue(group, 'energy_day_out') : null) ?? 0);
  const avg = dayAvgRate(sys);
  const cost = isInput ? Math.round((sys?.cost_day ?? 0) * 100) / 100 : avg.buy != null && inWh > 0 ? Math.round((inWh / 1000) * avg.buy * 100) / 100 : 0;
  const rev = isInput ? Math.round((sys?.rev_day ?? 0) * 100) / 100 : 0;
  const today = useMemo(() => (inWh >= 0 ? { inWh, outWh, cost, revenue: rev, n: 1 } : null), [inWh, outWh, cost, rev]);
  const v = usePeriodTotals(id, period, { today });
  useEffect(() => { onTotals(k, v); }, [k, v, onTotals]);
  useEffect(() => { onToday?.(k, today); }, [k, today, onToday]);
  return null;
};

/** Today so far from the live counters — the running day's bar of a day-step chart. */
interface TodaySoFar { inWh: number; outWh: number; cost: number; revenue: number }

/** The running day as a record of the chart's family (interval-start `ts`, `dur` = elapsed, so it
 *  reads as partial — «19 h 30 min of 24 h»), like the running hour on «Today». */
function todayRecord(t: TodaySoFar, type: 'electrical' | 'energy' | 'economics', clock: DeviceClock): HistoryRecord | null {
  const now = Math.floor(Date.now() / 1000);
  const ts = clock.dayStart(now);
  const dur = now - ts;
  if (type === 'energy') return { ts, dur, q: 0, res: 1, ec: t.inWh, es: t.outWh, en: t.inWh - t.outWh } as HistoryRecord;
  if (type === 'economics') return { ts, dur, q: 0, res: 0, c: t.cost, r: t.revenue, n: Math.round((t.cost - t.revenue) * 100) / 100 } as HistoryRecord;
  return null;
}

/** «16 окт.», «10–16 окт.», «сентябрь 2026», «2025» — the period on display, in the hub's time. */
function periodLabel(l: { from: number; to: number; kind: 'day' | 'week' | 'month' | 'year' }, clock: DeviceClock): string {
  const f = (ts: number, o: Intl.DateTimeFormatOptions) => formatDeviceTime(clock, ts, o, dateLocale());
  if (l.kind === 'day') return f(l.from, { day: 'numeric', month: 'short' });
  if (l.kind === 'week') return `${f(l.from, { day: 'numeric', month: 'short' })} – ${f(Math.max(l.from, l.to - 1), { day: 'numeric', month: 'short' })}`;
  if (l.kind === 'month') return f(l.from + 3600, { month: 'long', year: 'numeric' });
  return f(l.from + 3600, { year: 'numeric' });
}

export const HistoryPage: React.FC = () => {
  const hardware = useHardware(); // BASIC points: consumption only (table columns)
  const ds = useDsTokens();
  const caps = useCapabilities();
  const { data, conn: linkConn } = useTelemetryEntities();
  const linkLost = linkConn === 'error';

  // Selectable points (colour follows the entity, stable by unit key — §7.2): the live frame's
  // units plus every configured point — the archive does not need a live channel (rerun 16.10
  // SCADA-R03: «Точки недоступны» while the hub's history was fine, and 8 s on .139 after a jump).
  const unitsShadow = useConfigShadow('units');
  const joinEarly = useUnitsConfig();
  const points = useMemo(() => {
    const live = data?.units ?? [];
    const cfg = Array.isArray(unitsShadow.reported.units) ? (unitsShadow.reported.units as JsonObject[]) : [];
    // Live keys and their config HEX4 (a legacy hub keys the input "main" but configures 0006).
    const have = new Set<string>();
    live.forEach((g) => { have.add(g.unitKey.toUpperCase()); const h = joinEarly.forGroup(g)?.hex; if (h) have.add(h.toUpperCase()); });
    const extra: UnitEntityGroup[] = [];
    for (const u of cfg) {
      const k = hexKey(u.unit_id);
      if (!k || have.has(k.toUpperCase())) continue;
      extra.push({ unitKey: k, deviceId: k, name: String(u.name || u.short_name || k), present: false, srcClass: '', hasDirection: false, entities: [], attributes: {} } as unknown as UnitEntityGroup);
    }
    return [...live, ...extra];
  }, [data, unitsShadow.reported, joinEarly]);
  const orderedKeys = useMemo(() => points.map((p) => p.unitKey).sort(), [points]);
  const colorIndex = useCallback((k: string) => orderedKeys.indexOf(k), [orderedKeys]);
  const colorFor = useCallback(
    (k: string) => { const i = colorIndex(k); return i >= 0 && i < MAX_SERIES ? ds.series[i] : ds.inkMuted; },
    [colorIndex, ds.series, ds.inkMuted]
  );
  const selectable = useCallback((k: string) => colorIndex(k) < MAX_SERIES, [colorIndex]);
  // One name per point, the same as on the Overview (UX audit T6 — chips read «RBGrid Unit 0006»,
  // «floor1.kitchen»).
  const unitsCfg = useUnitsConfig();
  const gridSys = useGridSystem(unitsCfg);
  const nameFor = useCallback(
    (k: string) => {
      const g = points.find((p) => p.unitKey === k);
      return g ? fullDisplayName(g, unitsCfg.forGroup(g)) : t('history.pointFallback', { key: k });
    },
    [points, unitsCfg]
  );

  const [selected, setSelected] = useState<string[]>([]);
  // A WINDOW («вчера», «прошлый месяц»), the step follows from it (W2-1: RES-05).
  // Period, metric and view survive Back for the tab's session (L3P-21: Yesterday → a point → Back
  // landed on Today).
  const [win, setWin] = useSessionData<HistWindow>('rbgrid.history_win', 'today');
  const clock = useDeviceClock();
  // Running-period totals per selected point, the Economics way (HA-R04) — probes report up.
  const [periodTotals, setPeriodTotals] = useState<Record<string, PeriodTotals | null>>({});
  const onPeriodTotals = useCallback((k: string, v: PeriodTotals | null) => setPeriodTotals((m) => (m[k] === v ? m : { ...m, [k]: v })), []);
  const [todaySoFar, setTodaySoFar] = useState<Record<string, TodaySoFar | null>>({});
  const onToday = useCallback((k: string, v: TodaySoFar | null) => setTodaySoFar((m) => (m[k] === v ? m : { ...m, [k]: v })), []);
  // ‹ › steps the window back / forward by its period (HA-R10); a new window starts at «now».
  const [back, setBack] = useState(0);
  useEffect(() => { setBack(0); }, [win]);
  const span = useMemo(() => windowSpanAt(win, back, Math.floor(Date.now() / 1000), clock), [win, back, clock]);
  const period = span.step;
  const [prevResults, setPrevResults] = useState<Record<string, PointResult>>({});
  const [metricKey, setMetricKey] = useSessionData<ViewMetric>('rbgrid.history_metric', 'energy');
  const [view, setView] = useSessionData<'chart' | 'table'>('rbgrid.history_view', 'chart');
  // Bars: «total and parts» (the input split into its selected lines + the rest) or side by side.
  const [barMode, setBarMode] = useSessionData<'stack' | 'side'>('rbgrid.history_bars', 'stack');
  // With several points: which one shows its previous period ('' = none; charts audit §6).
  const [cmpWith, setCmpWith] = useSessionData<string>('rbgrid.history_cmp', '');
  const [results, setResults] = useState<Record<string, PointResult>>({});
  const [exporting, setExporting] = useState(false);

  const metric = useMemo(() => METRICS.find((m) => m.value === metricKey) ?? METRICS[0], [metricKey]);
  const totalsMetric = metric.value === 'energy' || metric.value === 'money';

  // Default-select the first point once telemetry arrives.
  useEffect(() => {
    if (!selected.length && orderedKeys.length) setSelected([orderedKeys[0]]);
  }, [orderedKeys, selected.length]);

  // Opened from another chart's «As a table» (?point=&win=&metric=&view=): applied once the
  // points are known, then dropped from the address so Back/refresh keep the user's own choice.
  const [params, setParams] = useSearchParams();
  useEffect(() => {
    const l = parseHistoryLink(params);
    if (!l) return;
    if (l.point) {
      if (!points.length) return;
      const want = l.point.toUpperCase();
      const g = points.find((p) => p.unitKey.toUpperCase() === want || joinEarly.forGroup(p)?.hex?.toUpperCase() === want);
      if (g) setSelected([g.unitKey]);
    }
    if (l.win) setWin(l.win);
    if (l.metric) setMetricKey(l.metric);
    if (l.view) setView(l.view);
    setParams({}, { replace: true });
  // eslint-disable-next-line react-hooks/exhaustive-deps -- the session setters change identity per value
  }, [params, points, joinEarly]);

  const togglePoint = (k: string) => {
    if (!selectable(k)) return;
    setSelected((prev) => (prev.includes(k) ? prev.filter((x) => x !== k) : [...prev, k]));
  };

  const onResult = useCallback((key: string, r: PointResult) => {
    setResults((prev) => ({ ...prev, [key]: r }));
  }, []);
  const onPrevResult = useCallback((key: string, r: PointResult) => {
    setPrevResults((prev) => ({ ...prev, [key]: r }));
  }, []);

  // Fetch key: metric's history family changes remount the fetchers.
  // A running week / month by days draws today too — from the live counters, as a partial day (the
  // period total already included it while its bar was missing; media pass 02.11).
  const selSeries = useMemo(
    () => selected.map((k) => {
      const recs = results[k]?.records ?? [];
      const td = back === 0 && period === 'day' && win !== 'today' && totalsMetric ? todaySoFar[k] : null;
      const rec = td ? todayRecord(td, metric.historyType, clock) : null;
      const records = rec && rec.ts >= span.from && rec.ts < span.to && !recs.some((r) => r.ts === rec.ts) ? [...recs, rec] : recs;
      return { key: k, color: colorFor(k), records };
    }),
    [selected, colorFor, results, back, period, win, totalsMetric, todaySoFar, metric.historyType, clock, span.from, span.to]
  );
  // The selected input and whether «total and parts» applies (the input + at least one line).
  const inputKey = selected.find((k) => { const g = points.find((p) => p.unitKey === k); return !!g && joinEarly.forGroup(g)?.role === 'MAIN_SUPPLY'; });
  const canStack = !!inputKey && selected.length > 1;

  const loading = selected.some((k) => !results[k] || results[k].loading);
  const [slowLoad, setSlowLoad] = useState(false);
  useEffect(() => {
    if (!loading) { setSlowLoad(false); return undefined; }
    const id = setTimeout(() => setSlowLoad(true), 12_000);
    return () => clearTimeout(id);
  }, [loading]);
  const error = selected.map((k) => results[k]?.error).find(Boolean) ?? null;
  const allRecords = useMemo(() => selSeries.flatMap((s) => s.records), [selSeries]);
  const hasData = allRecords.length > 0;

  const hasPartial = useMemo(() => allRecords.some((r) => isPartial(r, period, clock)), [allRecords, period, clock]);

  // Currency for «Деньги»: the records' own ISO `cur` when present, else the device's
  // schema.money (never a hardcoded ₽). Money on the wire is float MAJOR units everywhere.
  const recordCur = useMemo(
    () => allRecords.map((r) => (r as unknown as Record<string, unknown>).cur).find((c) => typeof c === 'string') as string | undefined,
    [allRecords],
  );
  const mf = useMoneyFormat(recordCur);

  // Value accessors + Y formatter per metric (display units).
  const accessors = useMemo(() => {
    switch (metric.value) {
      case 'power': return { kind: 'line' as Kind, valueOf: (r: HistoryRecord) => num(r, 'p_act'), fmtY: (v: number) => pwW(v) };
      case 'voltage': return {
        kind: 'line' as Kind, valueOf: (r: HistoryRecord) => num(r, 'v'), fmtY: (v: number) => volt(v),
        bandOf: (r: HistoryRecord): [number, number] | null => { const lo = num(r, 'v_min'), hi = num(r, 'v_max'); return lo != null && hi != null && hi >= lo ? [lo, hi] : null; },
      };
      case 'reactive': return { kind: 'line' as Kind, valueOf: (r: HistoryRecord) => num(r, 'p_rea'), fmtY: (v: number) => reactive(v) };
      case 'money': return {
        kind: 'bars' as Kind,
        valueOf: (r: HistoryRecord) => num(r, 'c'),
        upOf: (r: HistoryRecord) => num(r, 'c'),
        downOf: (r: HistoryRecord) => num(r, 'r'),
        fmtY: (v: number) => mf.money(v),
      };
      case 'energy':
      default: return {
        kind: 'bars' as Kind,
        // Energy is ALWAYS float Wh on the wire (canon fc0f962b); mWh exists only inside flash.
        valueOf: (r: HistoryRecord) => num(r, 'ec'),
        upOf: (r: HistoryRecord) => num(r, 'ec'),
        downOf: (r: HistoryRecord) => num(r, 'es'),
        fmtY: (v: number) => enWh(v),
      };
    }
  }, [metric.value, mf]);
  // The previous period is drawn for ONE point (several ghosts on several lines would be noise):
  // the only one selected, or the input in «total and parts» (charts audit C4).
  // With several side by side the user picks the point («Compare with the previous period»).
  const stackShown = canStack && barMode === 'stack' && accessors.kind !== 'line';
  const ghostKey = selected.length === 1 ? selected[0] : stackShown ? inputKey : selected.includes(cmpWith) ? cmpWith : undefined;

  // CSV size estimate: records × ≈ row bytes (§7.6).
  const csvEstimate = useMemo(() => {
    const bytes = allRecords.length * 64 + 96;
    return bytes >= 1024 ? t('history.sizeKb', { n: Math.round(bytes / 1024) }) : t('history.sizeB', { n: bytes });
  }, [allRecords.length]);

  // One file for the selected points and the window, from the data on screen (W2-2: RES-08/09/10
  // — a file per point, unix time, Wh and «,» used to land in one Excel column).
  const [exportErr, setExportErr] = useState<string | null>(null);
  // «Сводка по точкам» (RES-R11): kWh and money per selected point for the period on screen —
  // one row each, for re-billing tenants; both families fetched on demand.
  const exportSummary = async () => {
    if (!selected.length) return;
    setExporting(true);
    setExportErr(null);
    try {
      const rows = await Promise.all(selected.map(async (k) => {
        const g = points.find((p) => p.unitKey === k);
        const id = (g && joinEarly.forGroup(g)?.hex) || k;
        const get = (type: 'energy' | 'economics') => getApiClient().getHistory({ unit_id: id, period: span.step, type, offset: 0, limit: span.limit, ...wireRange(span.from, span.to, span.step) })
          .then((r) => toIntervalStarts(r.data ?? [], span.step)).catch(() => [] as HistoryRecord[]);
        const [e, m] = isDemo()
          ? [toIntervalStarts(demoHistory('energy', span.step, 48, id), span.step), toIntervalStarts(demoHistory('economics', span.step, 48, id), span.step)]
          : await Promise.all([get('energy'), get('economics')]);
        const te = windowTotals(e, span), tm = windowTotals(m, span);
        const input = !!g && joinEarly.forGroup(g)?.role === 'MAIN_SUPPLY';
        // A running period (today / this week / month / year): the same figures as the cards on
        // screen — closed records + today by the hub's day counters (H3P-R01: the file said
        // 6.749 kWh · €2.28 for «today» while every screen said 5.3 kWh · €1.93, and the week
        // left today out).
        const live = back === 0 && ['today', 'week', 'thisMonth', 'year'].includes(win) ? periodTotals[k] : null;
        if (live && !live.loading && live.n > 0) return { name: nameFor(k), inWh: live.inWh, outWh: live.outWh, cost: live.cost, revenue: live.revenue, input };
        return { name: nameFor(k), inWh: te.inWh, outWh: te.outWh, cost: tm.cost, revenue: tm.revenue, input };
      }));
      const text = buildSummaryCsv(rows, recordCur ?? caps.money.currencyCode ?? '', periodLabel(span.label, clock));
      const url = URL.createObjectURL(new Blob([text], { type: 'text/csv;charset=utf-8' }));
      const a = document.createElement('a');
      a.href = url;
      a.download = `rbgrid_${t('history.summaryFile')}_${periodLabel(span.label, clock)}.csv`.replace(/\s+/g, '_');
      a.click();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
    } catch {
      setExportErr(t('history.exportFailed'));
    } finally {
      setExporting(false);
    }
  };

  const exportCsv = () => {
    if (!selected.length || !hasData) return;
    setExporting(true);
    setExportErr(null);
    try {
      const { text, rows } = buildHistoryCsv(
        selSeries.map((s) => ({ name: nameFor(s.key), records: s.records })),
        metric.value, clock, recordCur ?? caps.money.currencyCode ?? '', span,
        period === 'hour' ? 3600 : period === 'day' ? 86400 : undefined,
      );
      if (!rows) { setExportErr(t('history.exportEmpty')); return; }
      const url = URL.createObjectURL(new Blob([text], { type: 'text/csv;charset=utf-8' }));
      const a = document.createElement('a');
      a.href = url;
      a.download = `rbgrid_${t(HIST_WINDOWS.find((w) => w.value === win)?.label ?? '')}_${t(metric.label)}.csv`.replace(/\s+/g, '_');
      a.click();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
    } catch {
      setExportErr(t('history.exportFailed'));
    } finally {
      setExporting(false);
    }
  };

  return (
    <Box>
      {back === 0 && ['today', 'week', 'thisMonth', 'year'].includes(win) && totalsMetric && selected.map((k) => {
        const g = points.find((p) => p.unitKey === k);
        return <TotalsProbe key={`pt-${k}-${win}`} k={k} id={(g && joinEarly.forGroup(g)?.hex) || k} group={g}
          period={win === 'today' ? 'day' : win === 'week' ? 'week' : win === 'thisMonth' ? 'month' : 'year'} isInput={!!g && joinEarly.forGroup(g)?.role === 'MAIN_SUPPLY'}
          sys={data?.system.totals} onTotals={onPeriodTotals} onToday={onToday} />;
      })}
      {/* hidden fetchers: one per selected point */}
      {selected.map((k) => (
        <PointFetcher key={`${k}-${metric.historyType}-${win}-${back}`} unitKey={k} range={span} limit={span.limit}
          period={period} historyType={metric.historyType} onResult={onResult} />
      ))}
      {/* the comparison window — totals only (energy / money) */}
      {selected.filter((k) => totalsMetric || k === ghostKey).map((k) => (
        <PointFetcher key={`prev-${k}-${metric.historyType}-${win}-${back}`} unitKey={k} range={span.prevFull ?? span.prev} limit={span.limit}
          period={period} historyType={metric.historyType} onResult={onPrevResult} />
      ))}

      {/* Header wraps on phones: «Скачать CSV» drops under the title instead of overflowing. */}
      <Stack direction="row" alignItems="center" flexWrap="wrap" columnGap={1.5} rowGap={1} mb={2.5} sx={{ minWidth: 0 }}>
        <Typography variant="h4" fontWeight={800} sx={{ color: ds.ink }}>{t('history.title')}</Typography>
        {hasPartial && <><StatusChip status="dormant" label={t('history.hasPartial')} /><HelpTip topic="partial" /></>}
        <Button
          startIcon={<DownloadIcon />}
          onClick={exportCsv}
          disabled={exporting || !selected.length || !hasData}
          sx={{ ml: 'auto', whiteSpace: 'nowrap', flexShrink: 0 }}
          title={!hasData ? t('history.exportEmpty') : undefined}
        >
          {exporting ? t('history.exporting') : t('history.downloadCsv', { size: csvEstimate })}
        </Button>
        <Button onClick={exportSummary} disabled={exporting || !selected.length} sx={{ whiteSpace: 'nowrap', flexShrink: 0 }}>
          {t('history.summaryCsv')}
        </Button>
      </Stack>

      {/* §7.2 — point chips, each carrying its series colour */}
      {/* Phones: one scrolling row, not half a screen of chips before the data (H3P-21). */}
      <Stack direction="row" gap={1} mb={1.5} sx={{ flexWrap: { xs: 'nowrap', sm: 'wrap' }, overflowX: { xs: 'auto', sm: 'visible' }, pb: { xs: 0.5, sm: 0 }, '& > *': { flexShrink: 0 } }}>
        {points.length === 0 && (
          <Typography variant="body2" sx={{ color: ds.inkMuted }}>{t('history.noPoints')}</Typography>
        )}
        {points.map((p) => {
          const on = selected.includes(p.unitKey);
          const can = selectable(p.unitKey);
          const c = colorFor(p.unitKey);
          return (
            <Chip
              key={p.unitKey}
              label={nameFor(p.unitKey)}
              disabled={!can}
              onClick={() => togglePoint(p.unitKey)}
              variant={on ? 'filled' : 'outlined'}
              icon={<Box component="span" sx={{ width: 10, height: 10, borderRadius: '50%', bgcolor: c, ml: 1 }} />}
              sx={{
                maxWidth: '100%',
                color: on ? ds.onPrimary : ds.ink,
                bgcolor: on ? c : 'transparent',
                borderColor: c,
                '& .MuiChip-label': { fontWeight: on ? 700 : 500 },
              }}
            />
          );
        })}
      </Stack>
      {points.length > MAX_SERIES && (
        <Typography variant="caption" sx={{ color: ds.inkMuted, display: 'block', mb: 1.5 }}>
          {t('history.firstPointsShown', { count: MAX_SERIES })}
        </Typography>
      )}

      {/* §7.3 period · §7.4 type · §7.5 view */}
      {/* Toggles: on phones each group is a full-width segment row that scrolls sideways
          when its labels don't fit (never widens the page). */}
      <Stack direction="row" flexWrap="wrap" gap={1.5} alignItems="center" mb={2} sx={{ minWidth: 0 }}>
        <ToggleButtonGroup size="small" exclusive value={win} sx={segmentSx}
          onChange={(_, v) => v && setWin(v as HistWindow)}>
          {HIST_WINDOWS.map((w) => <ToggleButton key={w.value} value={w.value}>{t(w.label)}</ToggleButton>)}
        </ToggleButtonGroup>
        <Stack direction="row" alignItems="center" spacing={0.5}>
          <IconButton size="small" aria-label={t('history.periodBack')} onClick={() => setBack((b) => b + 1)} sx={{ minWidth: 44, minHeight: 44 }}><ChevronLeftIcon /></IconButton>
          <Typography sx={{ fontSize: 13, minWidth: 120, textAlign: 'center', fontVariantNumeric: 'tabular-nums' }}>{periodLabel(span.label, clock)}</Typography>
          <IconButton size="small" aria-label={t('history.periodForward')} disabled={back === 0} onClick={() => setBack((b) => Math.max(0, b - 1))} sx={{ minWidth: 44, minHeight: 44 }}><ChevronRightIcon /></IconButton>
        </Stack>
        <ToggleButtonGroup size="small" exclusive value={metricKey} sx={segmentSx}
          onChange={(_, v) => v && setMetricKey(v as ViewMetric)}>
          {METRICS.map((m) => <ToggleButton key={m.value} value={m.value}>{t(m.label)}</ToggleButton>)}
        </ToggleButtonGroup>
        <ToggleButtonGroup size="small" exclusive value={view}
          onChange={(_, v) => v && setView(v as 'chart' | 'table')} sx={{ ...segmentSx, ml: { sm: 'auto' } }}>
          <ToggleButton value="chart">{t('history.viewChart')}</ToggleButton>
          <ToggleButton value="table">{t('history.viewTable')}</ToggleButton>
        </ToggleButtonGroup>
      </Stack>

      {exportErr && <Alert severity="warning" onClose={() => setExportErr(null)} sx={{ mb: 1.5 }}>{exportErr}</Alert>}
      {/* v1.1.2: a selected point the hub keeps no history for — say so instead of an empty chart. */}
      {selected.map((k) => {
        const g = points.find((p) => p.unitKey === k);
        const c = g ? joinEarly.forGroup(g) : undefined;
        return c?.history === false ? <NoHistoryNote key={k} history={false} historyCount={joinEarly.historyCount} name={c.name ?? g?.name} /> : null;
      })}
      {/* A restart with downtime inside the window: its hours may be missing here, «сегодня» on the
          Overview still counts them (root: the day counters survive) — say so instead of leaving a
          silent hole (rerun 16.10 RES-R02/HOME-R14). */}
      {(() => {
        const b = data?.system.totals?.boot;
        if (!b?.ts || !b.downtime_s || b.downtime_s < 60 || b.ts < span.from || b.ts - b.downtime_s > span.to) return null;
        return (
          <Alert severity="info" sx={{ mb: 1.5 }}>
            {t('history.bootGap', {
              from: formatDeviceTime(clock, b.ts - b.downtime_s, { day: 'numeric', month: 'short', hour: '2-digit', minute: '2-digit' }, dateLocale()),
              to: formatDeviceTime(clock, b.ts, { hour: '2-digit', minute: '2-digit' }, dateLocale()),
            })}
          </Alert>
        );
      })()}
      {/* Totals of the window + change vs the comparison window (W2-1: RES-06 — the answer to
          «сколько я потратил вчера / в прошлом месяце» used to need summing rows by hand). */}
      {totalsMetric && selected.length > 0 && !loading && (
        <Card>
          <Stack direction="row" columnGap={4} rowGap={1.5} flexWrap="wrap" useFlexGap alignItems="baseline" sx={{ mb: 0.5 }}>
            {selected.map((k) => {
              const closed = windowTotals(results[k]?.records ?? [], span);
              // The running week / month / year shows what Economics shows — closed records + today
              // (+ this month's days for the year) — HA-R04: History's month lacked today.
              // Today too: the hub's day counters, as on the Overview (3φ audit H3P-06: 3,2 vs 3,7 kWh).
              const live = back === 0 && ['today', 'week', 'thisMonth', 'year'].includes(win) ? periodTotals[k] : null;
              const cur = live && live.n > 0 ? live : closed;
              const prv = windowTotals(prevResults[k]?.records ?? [], span.prev);
              const money = metric.value === 'money';
              // A generation point leads with what it PRODUCED (GEN-R08: «отдано» meant two things).
              const g = points.find((p) => p.unitKey === k);
              const gen = !!g && isGenerationRole(unitsCfg.forGroup(g)?.role) && !money;
              const curV = money ? cur.cost : gen ? cur.outWh : cur.inWh;
              const prvV = money ? prv.cost : gen ? prv.outWh : prv.inWh;
              // The change compares CLOSED parts only (today has no counterpart in the span before);
              // only against a complete comparison span (RES-R04: «+885 %» against a half-recorded week).
              const closedV = money ? closed.cost : gen ? closed.outWh : closed.inWh;
              const d = prv.n > 0 && prv.n >= closed.n ? deltaPct(closedV, prvV) : null;
              return (
                <Box key={k} sx={{ minWidth: 0 }}>
                  {selected.length > 1 && <Typography sx={{ fontSize: 12, color: ds.inkMuted }} noWrap>{nameFor(k)}</Typography>}
                  <Typography sx={{ fontSize: 12, color: ds.inkMuted }}>{money ? t('history.totalCost') : gen ? t('history.totalGen') : t('history.totalIn')}</Typography>
                  <Typography sx={{ fontSize: 22, fontWeight: 700, color: gen ? ds.solarInk : ds.ink, fontVariantNumeric: 'tabular-nums' }}>
                    {cur.n ? (money ? mf.money(cur.cost) : enWh(curV)) : '—'}
                  </Typography>
                  {live && live.n > 0 && <Typography sx={{ fontSize: 11, color: ds.inkMuted }}>{t(win === 'today' ? 'history.todayCounters' : 'history.inclToday')}</Typography>}
                  {cur.n > 0 && !gen && (money ? cur.revenue > 0 : cur.outWh > 0) && (
                    <Typography sx={{ fontSize: 13, color: ds.flowExport }}>
                      {money ? t('history.totalRevenue', { v: mf.money(cur.revenue) }) : t('history.totalOut', { v: enWh(cur.outWh) })}
                    </Typography>
                  )}
                  <Typography sx={{ fontSize: 12, color: ds.inkMuted }}>
                    {d == null ? t('history.noCompare')
                      : Math.abs(d) < 0.5 ? t('history.deltaSame', { than: t(span.prevLabel) })
                      : t(d >= 0 ? 'history.deltaMore' : 'history.deltaLess', { pct: Math.abs(d).toLocaleString(intlLocale(), { maximumFractionDigits: 0 }), than: t(span.prevLabel) })}
                  </Typography>
                </Box>
              );
            })}
          </Stack>
        </Card>
      )}
      <Box sx={{ height: 12 }} />

      <Card>
        {error && <Alert severity="warning" sx={{ mb: 2 }}>{t('history.loadFailed')}</Alert>}
        {loading && selected.length > 0 && (
          <Box sx={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 1.5, py: 6 }}>
            <CircularProgress aria-label={t('common.loading')} />
            {/* A spinner alone for minutes read as «broken» while the hub restarted (I3P-13). */}
            {(slowLoad || linkLost) && <Typography sx={{ color: ds.inkMuted, fontSize: 13, textAlign: 'center' }}>{t(linkLost ? 'history.waitLink' : 'history.slow')}</Typography>}
          </Box>
        )}
        {!loading && !error && selected.length === 0 && (
          <Typography sx={{ color: ds.inkMuted, py: 6, textAlign: 'center' }}>{t('history.selectPoint')}</Typography>
        )}
        {!loading && !error && selected.length > 0 && !hasData && (
          <Typography sx={{ color: ds.inkMuted, py: 6, textAlign: 'center' }}>{t('history.noDataForPeriod')}</Typography>
        )}

        {!loading && !error && hasData && view === 'chart' && ((accessors.kind !== 'line' && canStack) || (selected.length > 1 && !stackShown)) && (
          <Stack direction="row" justifyContent="flex-end" alignItems="center" spacing={1} flexWrap="wrap" useFlexGap sx={{ mb: 1 }}>
            {selected.length > 1 && !stackShown && (
              <TextField select size="small" label={t('history.cmpWith')} value={selected.includes(cmpWith) ? cmpWith : ''}
                onChange={(e) => setCmpWith(e.target.value)} sx={{ minWidth: 260 }}>
                <MenuItem value="">{t('history.cmpNone')}</MenuItem>
                {selected.map((k) => <MenuItem key={k} value={k}>{nameFor(k)}</MenuItem>)}
              </TextField>
            )}
            {accessors.kind !== 'line' && canStack && (
              <ToggleButtonGroup size="small" exclusive value={barMode} onChange={(_, v) => v && setBarMode(v as 'stack' | 'side')} sx={segmentSx}>
                <ToggleButton value="stack">{t('history.barsStack')}</ToggleButton>
                <ToggleButton value="side">{t('history.barsSide')}</ToggleButton>
              </ToggleButtonGroup>
            )}
          </Stack>
        )}
        {!loading && !error && hasData && view === 'chart' && (() => {
          // The legend above the chart (charts audit B4: the point chips are a switch, not a key;
          // nothing said what «down» or the hatching meant).
          const anyDown = accessors.kind !== 'line' && 'downOf' in accessors && !!accessors.downOf
            && selSeries.some((sr) => sr.records.some((r) => ((accessors.downOf as (x: HistoryRecord) => number | null)(r) ?? 0) > 0));
          const anyPart = selSeries.some((sr) => sr.records.some((r) => isPartial(r, period, clock)));
          const stackOn = accessors.kind !== 'line' && canStack && barMode === 'stack';
          const items = [
            ...(stackOn
              ? [...selSeries.filter((sr) => sr.key !== inputKey && sr.records.some((r) => ((('upOf' in accessors && accessors.upOf) || accessors.valueOf)(r) ?? 0) > 0)).map((sr) => ({ label: nameFor(sr.key), color: sr.color })), { label: t('history.restOfInput'), color: ds.inkMuted, mark: 'faded' as const }]
              : selSeries.length > 1 ? selSeries.map((sr) => ({ label: nameFor(sr.key), color: sr.color, mark: accessors.kind === 'line' ? 'line' as const : 'box' as const })) : []),
            ...(anyDown ? [{ label: t('legend.exportedDown'), color: ds.flowExport }] : []),
            // This period vs the previous one (charts audit C1): the key repeats the marks.
            ...(ghostKey && (prevResults[ghostKey]?.records?.length ?? 0) > 0 ? [
              // in «total and parts» the bar is the sections themselves — no «this period» swatch
              // with several side by side the points' own swatches already say «this period»
              ...(stackOn || selSeries.length > 1 ? [] : [{ label: t(`history.cmpCur_${back > 0 ? 'other' : win}`), color: colorFor(ghostKey), mark: accessors.kind === 'line' ? 'line' as const : 'box' as const }]),
              { label: t(`history.cmpPrev_${back > 0 ? 'other' : win}`) + (selSeries.length > 1 && !stackOn ? ` · ${nameFor(ghostKey)}` : ''), color: accessors.kind === 'line' ? colorFor(ghostKey) : ds.ink, mark: accessors.kind === 'line' ? 'ghost' as const : 'line' as const },
            ] : []),
            ...(anyPart ? [{ label: t('legend.partialPeriod'), color: ds.inkMuted, mark: accessors.kind === 'line' ? 'ring' as const : 'hatch' as const }] : []),
          ];
          return items.length ? <ChartLegend items={items} sx={{ mb: 1, justifyContent: 'flex-end' }} /> : null;
        })()}
        {!loading && !error && hasData && view === 'chart' && (
          <SvgTrend
            series={selSeries}
            kind={accessors.kind}
            period={period}
            valueOf={accessors.valueOf}
            upOf={'upOf' in accessors ? accessors.upOf : undefined}
            downOf={'downOf' in accessors ? accessors.downOf : undefined}
            bandOf={'bandOf' in accessors ? accessors.bandOf : undefined}
            fmtY={accessors.fmtY}
            curve={period === 'hour' && metric.value !== 'voltage' ? 'step' : 'monotoneX'}
            nominalV={metric.value === 'voltage' ? gridSys.nominalV : undefined}
            xRange={period === 'hour' && span.label.kind === 'day' ? { from: span.from, to: clock.dayStart(span.from + 1.5 * 86400) } : undefined}
            inputKey={inputKey}
            stacked={canStack && barMode === 'stack'}
            ghostLabel={t(`history.cmpPrev_${back > 0 ? 'other' : win}`) + (selected.length > 1 && ghostKey && !stackShown ? ` · ${nameFor(ghostKey)}` : '')}
            ghost={ghostKey && (prevResults[ghostKey]?.records?.length ?? 0) > 0 && span.prevFull
              ? { key: ghostKey, records: prevResults[ghostKey].records, offset: span.from - span.prevFull.from }
              : undefined}
            nameOf={nameFor}
          />
        )}
        {/* cos φ as its own strip under Q — one x axis, its own 0.5–1.0 scale, never a second y
            axis on the same chart (charts audit O11; the «Reactive · cos φ» tab showed only Q). */}
        {!loading && !error && hasData && view === 'chart' && metric.value === 'reactive' && selSeries.some((sr) => sr.records.some((r) => num(r, 'pf') != null)) && (
          <Box sx={{ mt: 1.5 }}>
            <Typography sx={{ fontSize: 12, color: ds.inkMuted, mb: 0.5 }}>{t('history.pfStrip')}</Typography>
            <SvgTrend
              series={selSeries}
              kind="line"
              period={period}
              valueOf={(r) => num(r, 'pf')}
              fmtY={(v) => nf(v, 2)}
              curve={period === 'hour' ? 'step' : 'monotoneX'}
              xRange={period === 'hour' && span.label.kind === 'day' ? { from: span.from, to: clock.dayStart(span.from + 1.5 * 86400) } : undefined}
              height={160}
              yWindow={[0.5, 1]}
              nameOf={nameFor}
              ghostLabel={t(`history.cmpPrev_${back > 0 ? 'other' : win}`) + (selected.length > 1 && ghostKey && !stackShown ? ` · ${nameFor(ghostKey)}` : '')}
              ghost={ghostKey && (prevResults[ghostKey]?.records?.length ?? 0) > 0 && span.prevFull
                ? { key: ghostKey, records: prevResults[ghostKey].records, offset: span.from - span.prevFull.from }
                : undefined}
            />
          </Box>
        )}

        {!loading && !error && hasData && view === 'table' && (
          <Stack spacing={3}>
            {selSeries.filter((s) => s.records.length > 0).map((s) => (
              <Box key={s.key}>
                {selected.length > 1 && (
                  <Stack direction="row" alignItems="center" spacing={1} mb={1}>
                    <Box sx={{ width: 10, height: 10, borderRadius: '50%', bgcolor: s.color }} />
                    <Typography variant="subtitle2" sx={{ color: ds.ink }}>{nameFor(s.key)}</Typography>
                  </Stack>
                )}
                <HistoryTable records={s.records} metric={metric.value} period={period}
                  fmtMoney={mf.money} ds={ds} consumptionOnly={pointHardware(s.key, hardware.sensors, hardware.modules).basic} />
              </Box>
            ))}
          </Stack>
        )}
      </Card>
    </Box>
  );
};

// ── table (§7.5) ─────────────────────────────────────────────────────────
const HistoryTable: React.FC<{
  records: HistoryRecord[];
  metric: ViewMetric;
  period: HistoryPeriod;
  fmtMoney: (v: number | null) => string;
  ds: ReturnType<typeof useDsTokens>;
  /** BASIC point (consumption only, root 18.10): no export / revenue / net columns. */
  consumptionOnly?: boolean;
}> = ({ records, metric, period, fmtMoney, ds, consumptionOnly }) => {
  const clock = useDeviceClock();
  const mono = { fontFamily: dsFonts.mono, fontVariantNumeric: 'tabular-nums' as const, whiteSpace: 'nowrap' as const };
  const head = { color: ds.inkMuted, fontWeight: 700, borderColor: ds.border };
  const xsHide = { display: { xs: 'none', sm: 'table-cell' } };
  const cell = { color: ds.ink, borderColor: ds.border };

  const cols: { label: string; align: 'left' | 'right' }[] =
    metric === 'energy' ? [
      { label: 'history.colConsumption', align: 'right' }, { label: 'history.colExport', align: 'right' }, { label: 'history.colNet', align: 'right' },
    ] : metric === 'money' ? [
      { label: 'history.colCost', align: 'right' }, { label: 'history.colRevenue', align: 'right' }, { label: 'history.colNet', align: 'right' },
    ] : metric === 'power' ? [
      { label: 'history.metricPower', align: 'right' }, { label: 'history.colMin', align: 'right' }, { label: 'history.colMax', align: 'right' },
    ] : metric === 'reactive' ? [
      { label: 'history.colReactive', align: 'right' }, { label: 'history.colPf', align: 'right' }, { label: 'history.colApparent', align: 'right' },
    ] : [
      { label: 'history.metricVoltage', align: 'right' }, { label: 'history.colMin', align: 'right' }, { label: 'history.colMax', align: 'right' },
    ];

  const oneCol = consumptionOnly && (metric === 'energy' || metric === 'money');
  // BASIC: every hour carries bit 64 (no direction) — said once above the table, not per row.
  const skip = consumptionOnly ? [6] : [];
  if (oneCol) cols.splice(1);
  const valuesOf = (r: HistoryRecord): string[] => (oneCol ? allValuesOf(r).slice(0, 1) : allValuesOf(r));
  const allValuesOf = (r: HistoryRecord): string[] => {
    const e = (wh: number | null) => enWh(wh);
    const m = (v: number | null) => fmtMoney(v);
    switch (metric) {
      case 'energy': return [e(num(r, 'ec')), e(num(r, 'es')), e(num(r, 'en'))];
      case 'money': return [m(num(r, 'c')), m(num(r, 'r')), m(num(r, 'n'))];
      case 'power': return [pwW(num(r, 'p_act')), pwW(num(r, 'p_min')), pwW(num(r, 'p_max'))];
      case 'reactive': { const pf = num(r, 'pf'), sva = num(r, 'p_app'); return [reactive(num(r, 'p_rea'), num(r, 'p_act')), pf != null && (sva == null || sva >= 1) ? nf(pf, 2) : '—', sva != null ? `${nf(sva)} ${t('units.VA')}` : '—']; }
      case 'voltage':
      default: return [volt(num(r, 'v')), volt(num(r, 'v_min')), volt(num(r, 'v_max'))];
    }
  };

  return (
    // Keyboard users must be able to scroll it (axe scrollable-region-focusable).
    <Box sx={{ overflowX: 'auto' }} tabIndex={0} role="region" aria-label={t('history.tableAria')}>
      {consumptionOnly && <Typography variant="caption" color="text.secondary" sx={{ display: 'block', mb: 1 }}>{t('history.basicNote')}</Typography>}
      <Table size="small">
        <TableHead>
          <TableRow>
            <TableCell sx={head}>{t('history.colTime')}</TableCell>
            {/* Phones (< 600): the 3rd figure and the quality column give way (RES-24: 517 px in a
                325 px screen); the quality moves under the time instead. */}
            {cols.map((c, j) => <TableCell key={c.label} align={c.align} sx={{ ...head, ...(j === 2 ? xsHide : {}) }}>{t(c.label)}</TableCell>)}
            <TableCell sx={{ ...head, ...xsHide }}>{t('history.colQuality')}</TableCell>
          </TableRow>
        </TableHead>
        <TableBody>
          {/* Newest first by the interval START (L3P-23: «17:00» above «17:02» — ordered by the end). */}
          {[...records].sort((a, b) => b.ts - a.ts).map((r, i) => {
            const partial = isPartial(r, period, clock);
            return (
              <TableRow key={`${r.ts}-${i}`} sx={partial ? { bgcolor: ds.sevAlarmSoft } : undefined}>
                <TableCell sx={{ ...cell, ...mono }}>
                  {formatRowTime(r.ts, period, clock, num(r, 'dur'))}
                  {qualityWord(r, period, clock, skip) !== t('history.qualityFull') && (
                    <Box component="span" sx={{ display: { xs: 'block', sm: 'none' }, fontFamily: 'inherit', fontSize: 11, color: partial ? ds.sevAlarm : ds.inkMuted, whiteSpace: 'normal' }}>
                      {qualityWord(r, period, clock, skip)}
                    </Box>
                  )}
                </TableCell>
                {valuesOf(r).map((v, j) => (
                  <TableCell key={j} align="right" sx={{ ...cell, ...mono, ...(j === 2 ? xsHide : {}) }}>{v}</TableCell>
                ))}
                <TableCell sx={{ ...cell, ...xsHide, color: partial ? ds.sevAlarm : ds.inkMuted }}>
                  {qualityWord(r, period, clock, skip)}
                </TableCell>
              </TableRow>
            );
          })}
        </TableBody>
      </Table>
    </Box>
  );
};
