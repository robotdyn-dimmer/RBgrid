/**
 * ParkingPowerChart — «Мощность паркинга · сегодня» (spec §4.3.4, review 04 §5.4 #4):
 * the parking power line (hourly average of `p_act`) with a horizontal input-limit line in
 * sev-critical. Drawn like OverviewEnergyChart: plain inline SVG, fixed height, real pixels
 * for the measured card width (`useElementWidth`), so 11 px labels stay 11 px.
 *
 * Data (`useParkingHistory`): history `electrical` hourly buckets since local midnight for
 * every parking line (stations + infrastructure, the SAME set the load-balance bar sums),
 * summed per bucket. With no lines or too many (> MAX_UNITS requests) it falls back to the
 * main input as a proxy and SAYS so in the title. The same fetch yields «Сессий сегодня»
 * (rising edges of a station's hourly average above the charge threshold) — an estimate,
 * labelled as such by the caller. A station made of several points (a three-phase charger, one
 * point per conductor) is counted on the SUM of its points, so one session counts once.
 */
import { linePath, runsOf } from '../charts/chartPath';
import React, { useEffect, useMemo, useState } from 'react';
import { Box, Typography } from '@mui/material';
import { Card } from '@rbgrid/design-system';
import { useDsTokens } from '../../theme/useDsTokens';
import { useElementWidth } from '../../hooks/useElementWidth';
import { getApiClient } from '../../services/api';
import { isDemo } from '../../services/connection';
import { demoHistory } from '../../mock/demo';
import { HistoryElectricalRecord } from '../../types/history';
import { toIntervalStarts } from '../../core/time/recordTime';
import { useDeviceClock, DeviceClock } from '../../core/deviceTime';
import { apiUnitId, MAIN_HEX_KEY } from '../../entities/siteModel';
import { nf } from '../common/ruFormat';
import { ChartLegend } from '../common/ChartLegend';
import { t } from '../../i18n';

/** Above this many lines the chart uses the main input instead (one request, not N). */
const MAX_UNITS = 8;
const REFRESH_MS = 5 * 60_000;
const HOUR = 3600;

export interface ParkingPoint { ts: number; w: number; partial: boolean; }

export interface ParkingHistory {
  points: ParkingPoint[];
  /** Station sessions started today (estimate from hourly buckets); null = unknown. */
  sessions: number | null;
  /** 'lines' = Σ parking lines; 'input' = main input proxy; null = nothing loaded yet. */
  source: 'lines' | 'input' | null;
  loading: boolean;
}

/** The hub's midnight (W1-8) — the day axis and «today» follow the hub's clock. */
const midnightSec = (clock: DeviceClock): number => clock.dayStart(Math.floor(Date.now() / 1000));

async function fetchToday(key: string, from: number): Promise<HistoryElectricalRecord[]> {
  let recs: HistoryElectricalRecord[];
  if (isDemo()) {
    recs = demoHistory('electrical', 'hour', 30, key) as unknown as HistoryElectricalRecord[];
  } else {
    const id = apiUnitId(key);
    if (!id) return [];
    const res = await getApiClient().getHistory<HistoryElectricalRecord>({
      unit_id: id, period: 'hour', type: 'electrical', offset: 0, limit: 30, from: from + 1,
    });
    recs = res.data ?? [];
  }
  return toIntervalStarts(recs, 'hour') // wire ts = the hour's END (canon ed876014)
    .filter((r) => typeof r.ts === 'number' && r.ts >= from && typeof r.p_act === 'number')
    .sort((a, b) => a.ts - b.ts);
}

/** Rising edges of the hourly average above the threshold = sessions started today. */
export function countSessions(recs: Pick<HistoryElectricalRecord, 'ts' | 'p_act'>[], thresholdW: number): number {
  let n = 0;
  let prevOn = false;
  for (const r of recs) {
    const on = Math.abs(r.p_act) > thresholdW;
    if (on && !prevOn) n += 1;
    prevOn = on;
  }
  return n;
}

/** One station's hourly series: its points' |P| summed per hub hour. */
export function stationSeries(perPoint: Pick<HistoryElectricalRecord, 'ts' | 'p_act'>[][], from: number): { ts: number; p_act: number }[] {
  const by = new Map<number, number>();
  for (const recs of perPoint) {
    for (const r of recs) {
      const b = from + Math.floor((r.ts - from) / HOUR) * HOUR;
      by.set(b, (by.get(b) ?? 0) + Math.abs(r.p_act));
    }
  }
  return Array.from(by.entries()).sort(([a], [b]) => a - b).map(([ts, p_act]) => ({ ts, p_act }));
}

export function useParkingHistory(
  lineKeys: string[], stationGroups: string[][], thresholdW: number, inputHex: string = MAIN_HEX_KEY,
): ParkingHistory {
  const [state, setState] = useState<ParkingHistory>({ points: [], sessions: null, source: null, loading: false });
  const clock = useDeviceClock();
  const linesSig = lineKeys.join(',');
  const stationsSig = stationGroups.map((g) => g.join('+')).join(',');

  useEffect(() => {
    let alive = true;
    const lines = linesSig ? linesSig.split(',') : [];
    const stations = stationsSig ? stationsSig.split(',').map((g) => g.split('+')) : [];
    const useInput = lines.length === 0 || lines.length > MAX_UNITS;
    const keys = useInput ? [inputHex] : lines;

    const load = async () => {
      setState((s) => ({ ...s, loading: true }));
      const from = midnightSec(clock);
      const results = await Promise.allSettled(keys.map((k) => fetchToday(k, from)));
      if (!alive) return;
      const byBucket = new Map<number, ParkingPoint>();
      let anyOk = false;
      const byKey = new Map<string, HistoryElectricalRecord[] | null>();
      results.forEach((res, idx) => {
        byKey.set(keys[idx], res.status === 'fulfilled' ? res.value : null);
        if (res.status !== 'fulfilled') return;
        anyOk = true;
        for (const r of res.value) {
          const b = from + Math.floor((r.ts - from) / HOUR) * HOUR; // hub hours (half-hour zones too)
          const cur = byBucket.get(b) ?? { ts: b, w: 0, partial: false };
          cur.w += Math.max(0, r.p_act);
          cur.partial = cur.partial || ((r.q ?? 0) & 1) === 1;
          byBucket.set(b, cur);
        }
      });
      let sessions: number | null = useInput ? null : 0;
      for (const st of stations) {
        const recs = st.map((k) => byKey.get(k));
        if (sessions == null || recs.some((r) => !r)) { sessions = null; break; }
        sessions += countSessions(stationSeries(recs as HistoryElectricalRecord[][], from), thresholdW);
      }
      setState({
        points: Array.from(byBucket.values()).sort((a, b) => a.ts - b.ts),
        sessions: anyOk ? sessions : null,
        source: anyOk ? (useInput ? 'input' : 'lines') : null,
        loading: false,
      });
    };
    void load();
    const id = setInterval(() => { void load(); }, REFRESH_MS);
    return () => { alive = false; clearInterval(id); };
  }, [linesSig, stationsSig, thresholdW, inputHex, clock]);

  return state;
}

const H = 200;
const PAD_L = 52;
const PAD_R = 10;
const PAD_T = 16;
const PAD_B = 22;
const HOUR_STRIDES = [1, 2, 3, 4, 6, 12];

const kwText = (w: number): string => `${nf(w / 1000, w >= 10_000 || w === 0 ? 0 : 1)}`;
/** kwText + the kW unit ("7,4 кВт"). */
const kwUnit = (w: number): string => `${kwText(w)} ${t('units.kW')}`;

/** A «nice» tick step (1/2/2,5/5 × 10^k W) giving ~3 gridlines up to `max`. */
function niceStep(max: number): number {
  const raw = max / 3;
  const pow = 10 ** Math.floor(Math.log10(raw));
  const m = [1, 2, 2.5, 5, 10].find((k) => k * pow >= raw) ?? 10;
  return m * pow;
}

export interface ParkingPowerChartProps {
  history: ParkingHistory;
  /** Input limit (W); undefined = not set → no limit line, said in the caption. */
  limitW?: number;
}

export const ParkingPowerChart: React.FC<ParkingPowerChartProps> = ({ history, limitW }) => {
  const ds = useDsTokens();
  const clock = useDeviceClock();
  const [measureRef, measured] = useElementWidth<HTMLDivElement>(760);
  const W = Math.max(240, measured);
  const { points, source } = history;

  const geo = useMemo(() => {
    const plotW = W - PAD_L - PAD_R;
    const plotH = H - PAD_T - PAD_B;
    const day0 = midnightSec(clock);
    const maxData = Math.max(0, ...points.map((p) => p.w));
    // The scale follows the data; a limit far above it (43 kW over 3.3 kW flattened the day —
    // charts audit O1) is marked at the top edge instead of stretching the axis.
    const limitFits = !!limitW && limitW <= Math.max(1000, maxData) * 1.5;
    const yMax = Math.max(1000, maxData * 1.15, limitFits && limitW ? limitW * 1.08 : 0);
    // x = the hour's start on a 0–24 h day axis; each hourly mean is held over its hour (a step).
    const x = (ts: number) => PAD_L + (((ts - day0) / HOUR) / 24) * plotW;
    const y = (w: number) => PAD_T + plotH - (w / yMax) * plotH;
    return { plotW, plotH, yMax, x, y, limitFits };
  }, [W, points, limitW, clock]);

  const stride = HOUR_STRIDES.find((k) => (geo.plotW / 24) * k >= 28) ?? 12;
  const runs = runsOf(points, (p) => p.ts, (p) => p.w, HOUR * 1.5);
  const runPath = (rs: typeof points) => linePath(rs.map((p) => ({ x: geo.x(p.ts), y: geo.y(p.w) })), 'step', geo.x(rs[rs.length - 1].ts + HOUR));
  const areaOf = (rs: typeof points) => `${runPath(rs)} L${geo.x(rs[rs.length - 1].ts + HOUR).toFixed(1)} ${geo.y(0).toFixed(1)} L${geo.x(rs[0].ts).toFixed(1)} ${geo.y(0).toFixed(1)} Z`;
  const peak = points.length ? points.reduce((m, p) => (p.w > m.w ? p : m)) : null;
  const step = niceStep(geo.yMax);
  const yTicks: number[] = [];
  for (let tk = 0; tk <= geo.yMax + 1e-6; tk += step) yTicks.push(tk);

  return (
    <Card>
      <Box sx={{ display: 'flex', justifyContent: 'space-between', alignItems: 'baseline', flexWrap: 'wrap', columnGap: 2, mb: 1 }}>
        <Typography sx={{ fontWeight: 700, fontSize: 15 }}>
          {t('stations.chartTitle')}{source === 'input' ? ` · ${t('stations.byInput')}` : ''}
        </Typography>
        <Typography sx={{ fontSize: 12, color: ds.inkMuted, fontVariantNumeric: 'tabular-nums' }}>
          {peak ? `${t('stations.peakAt', { power: kwUnit(peak.w), hour: clock.parts(peak.ts).h })} · ` : ''}
          {limitW ? t('stations.limit', { power: kwUnit(limitW) }) : t('stations.limitNotSet')} · {t('stations.hourlyAvg')}
        </Typography>
      </Box>
      {points.length === 0 ? (
        <Box sx={{ py: 4, textAlign: 'center', color: ds.inkMuted, fontSize: 13 }}>
          {history.loading ? t('common.loading') : t('stations.noDataToday')}
        </Box>
      ) : (
        <Box ref={measureRef} sx={{ position: 'relative', width: '100%', minWidth: 0, height: H, overflow: 'hidden',
          '& > svg': { display: 'block', position: 'absolute', left: 0, top: 0 } }}>
          <svg viewBox={`0 0 ${W} ${H}`} width={W} height={H} role="img"
            aria-label={[
              t('stations.chartAria'),
              ...(peak ? [t('stations.peak', { power: kwUnit(peak.w) })] : []),
              ...(limitW ? [t('stations.limit', { power: kwUnit(limitW) })] : []),
            ].join(', ')}>
            {yTicks.map((tk, i) => (
              <g key={i}>
                <line x1={PAD_L} x2={W - PAD_R} y1={geo.y(tk)} y2={geo.y(tk)} stroke={ds.chartGrid} strokeWidth="1" />
                <text x={PAD_L - 6} y={geo.y(tk) + 4} textAnchor="end" fill={ds.inkMuted} fontSize="11">
                  {kwText(tk)}{i === yTicks.length - 1 ? ` ${t('units.kW')}` : ''}
                </text>
              </g>
            ))}
            {runs.map((rs, i) => <path key={`a${i}`} d={areaOf(rs)} fill={ds.primary} opacity="0.1" />)}
            {runs.map((rs, i) => <path key={`l${i}`} d={runPath(rs)} fill="none" stroke={ds.primary} strokeWidth="2" strokeLinejoin="round" strokeLinecap="round" />)}
            {/* No dot on every hour (charts audit L5): a hover target per hour, a ring on a partial one. */}
            {points.map((p) => (
              <g key={p.ts}>
                <rect x={geo.x(p.ts)} y={PAD_T} width={Math.max(1, geo.x(p.ts + HOUR) - geo.x(p.ts))} height={geo.plotH} fill="transparent">
                  <title>{`${clock.parts(p.ts).h}:00 — ${kwUnit(p.w)}${p.partial ? ` ${t('stations.partialHour')}` : ''}`}</title>
                </rect>
                {p.partial && <circle cx={(geo.x(p.ts) + geo.x(p.ts + HOUR)) / 2} cy={geo.y(p.w)} r={3.5} fill={ds.surface} stroke={ds.primary} strokeWidth="1.5" />}
              </g>
            ))}
            {limitW != null && !geo.limitFits && (
              <text x={W - PAD_R} y={PAD_T + 10} textAnchor="end" fill={ds.sevCritical} fontSize="11" fontWeight="600">
                {t('stations.limitAbove', { power: kwUnit(limitW), pct: Math.round(((peak?.w ?? 0) / limitW) * 100) })}
              </text>
            )}
            {limitW != null && geo.limitFits && (
              <g>
                <line x1={PAD_L} x2={W - PAD_R} y1={geo.y(limitW)} y2={geo.y(limitW)}
                  stroke={ds.sevCritical} strokeWidth="1.5" strokeDasharray="6 4" />
                <text x={W - PAD_R} y={geo.y(limitW) - 5} textAnchor="end" fill={ds.sevCritical} fontSize="11" fontWeight="600">
                  {t('stations.limit', { power: kwUnit(limitW) })}
                </text>
              </g>
            )}
            {Array.from({ length: 24 }, (_, h) => h).filter((h) => h % stride === 0).map((h) => (
              <text key={h} x={PAD_L + (h / 24) * geo.plotW} y={H - 6} textAnchor="middle" fill={ds.inkMuted} fontSize="11">{h}</text>
            ))}
          </svg>
        </Box>
      )}
      {points.length > 0 && (
        <ChartLegend sx={{ mt: 1 }} items={[
          { label: t('legend.parkingPower'), color: ds.primary, mark: 'line' },
          { label: t('legend.partialHour'), color: ds.primary, mark: 'ring' },
          ...(limitW && geo.limitFits ? [{ label: t('legend.inputLimit'), color: ds.sevCritical, mark: 'dash' as const }] : []),
        ]} />
      )}
    </Card>
  );
};
