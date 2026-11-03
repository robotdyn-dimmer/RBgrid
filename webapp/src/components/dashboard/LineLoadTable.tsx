/**
 * LineLoadTable — the Предприятие «Пульт» line table (spec §4.2.2).
 *
 * One row per non-main line: name · phase · P (a single-CT multi-phase line: «≈» ×N estimate,
 * W9; I stays the measured conductor current) · I · energy today · load bar vs the line
 * nominal (`rated_power_w`, G3) · status. Status is shown ONLY on deviation (нет связи /
 * высокая / выше лимита / тревога) — a normal line has an empty cell (review 04 P0-2: no
 * sea of green «норма»). An offline line reads «—», never «0 Вт». A trailing «Остаток» row
 * (input − Σ lines) is optional: the Предприятие hero owns that number (one number — one
 * place), so the Overview passes `showRemainder={false}`. Row → the point.
 */
import React from 'react';
import { useNavigate } from 'react-router-dom';
import {
  Box, Table, TableBody, TableCell, TableHead, TableRow, Typography, Chip,
} from '@mui/material';
import { useDsTokens } from '../../theme/useDsTokens';
import { dsFonts } from '../../theme/dsTokens';
import { UnitEntityGroup, unitMetricValue } from '../../entities/model';
import { UnitsConfigJoin, phaseLabel } from '../../hooks/useUnitsConfig';
import { fullPower } from '../../entities/siteModel';
import { lineUnits, fullNameOf, isGenerationUnit } from '../../pages/overviewData';
import { useMoneyLens, useLensFormat } from '../../economics/MoneyLens';
import { moneyPerHour } from '../../economics/costRate';
import { t, intlLocale } from '../../i18n';

const nf = (n: number, frac = 0) =>
  n.toLocaleString(intlLocale(), { minimumFractionDigits: frac, maximumFractionDigits: frac });

/** Power W → "1,85 кВт" / "640 Вт". */
function fmtPower(w: number | null): string {
  if (w == null) return '—';
  const a = Math.abs(w);
  return a >= 1000 ? `${nf(w / 1000, 2)} ${t('units.kW')}` : `${nf(w)} ${t('units.W')}`;
}
function fmtCurrent(a: number | null): string {
  return a == null ? '—' : `${nf(a, 1)} ${t('units.A')}`;
}
/** Energy Wh → "3,2 кВт·ч". */
function fmtEnergy(wh: number | null): string {
  return wh == null ? '—' : `${nf(wh / 1000, 1)} ${t('units.kWh')}`;
}

interface Row {
  key: string;
  name: string;
  phase?: number;
  power: number | null;
  /** W9: `power` is the ×N balance estimate of a single-CT multi-phase line. */
  estimated: boolean;
  current: number | null;
  energyDay: number | null;
  ratedPowerW?: number;
  present: boolean;
  noChannel?: boolean;
  /** The CT at its limit (hub clip / ct_warn). */
  ct?: 'clip' | 'warn';
  isGeneration: boolean;
  alarm: boolean;
}

interface Status { label: string; color: string; }

export interface LineLoadTableProps {
  groups: UnitEntityGroup[];
  join: UnitsConfigJoin;
  /** Main-input P (W) — the «Остаток» minuend. */
  inputPowerW: number | null;
  /** Show the trailing «Остаток (неучтённое)» row (default true). */
  showRemainder?: boolean;
}

export const LineLoadTable: React.FC<LineLoadTableProps> = ({ groups, join, inputPowerW, showRemainder: remainderRow = true }) => {
  const ds = useDsTokens();
  const navigate = useNavigate();
  // Money per line (review 10): P × live buy rate (export × sell), only with an active tariff.
  const lens = useMoneyLens();
  const showMoney = lens.rate.buy != null;
  const perHour = (w: number | null) => {
    const m = moneyPerHour(w, lens.rate);
    return m == null ? '—' : `${m < 0 ? '+' : ''}${lens.fmt(Math.abs(m))}`;
  };

  const moneyFirst = showMoney && lens.mode === 'money';
  const fmtLens = useLensFormat();
  const moneyCell = (r: Row) => (
    <TableCell align="right" sx={{ ...mono, color: (r.power ?? 0) < 0 ? ds.flowExport : moneyFirst ? ds.ink : ds.inkMuted }}>
      {r.present ? perHour(r.power) : '—'}
    </TableCell>
  );

  const rows: Row[] = lineUnits(groups, join)
    .map((g) => {
      const cfg = join.forGroup(g);
      const full = fullPower(g, cfg);
      return {
        key: g.unitKey,
        name: fullNameOf(g, join),
        phase: cfg?.phase,
        power: full.w,
        estimated: full.estimated && full.w != null,
        current: g.present ? unitMetricValue(g, 'current') : null,
        // Generation's day = its OUTPUT (SCADA-R09: the sun read 0,0 kWh next to «выработано 0,5»).
        // An offline line that measured nothing today is «—», not «0.0 kWh» (3φ audit I3P-20: read as
        // «measured zero»); what it counted before dropping out still shows.
        energyDay: (() => { const v = unitMetricValue(g, isGenerationUnit(g, join) ? 'energy_day_out' : 'energy_day_in'); return g.present || (v ?? 0) > 0 ? v : null; })(),
        ratedPowerW: cfg?.ratedPowerW,
        present: g.present,
        noChannel: g.noChannel === true,
        isGeneration: isGenerationUnit(g, join),
        alarm: g.attributes?.['alarm'] === true,
        ct: g.present ? (g.attributes?.clip === true ? 'clip' as const : g.attributes?.ct_warn === true ? 'warn' as const : undefined) : undefined,
      };
    })
    .sort((a, b) => {
      if (a.present !== b.present) return a.present ? -1 : 1;
      return Math.abs(b.power ?? 0) - Math.abs(a.power ?? 0);
    });

  const sumLines = rows.reduce((s, r) => s + (r.power ?? 0), 0);
  const remainder = inputPowerW != null ? inputPowerW - sumLines : null;
  const showRemainder = remainderRow &&
    remainder != null && inputPowerW != null &&
    Math.abs(remainder) > Math.max(50, 0.05 * Math.abs(inputPowerW));

  const loadPct = (power: number | null, rated?: number): number | null =>
    power != null && rated && rated > 0 ? (Math.abs(power) / rated) * 100 : null;

  /** Deviation only; null = normal (nothing drawn). */
  const statusOf = (r: Row): Status | null => {
    if (!r.present) return { label: t(r.noChannel ? 'cards.noChannel' : 'cards.offline'), color: ds.statusOffline };
    if (r.alarm) return { label: t('cards.statusAlarm'), color: ds.sevAlarm };
    if (r.ct === 'clip') return { label: t('cards.ctClipShort'), color: ds.sevAlarm };
    if (r.ct === 'warn') return { label: t('cards.ctWarnShort'), color: ds.sevWarning };
    if (r.isGeneration) return null;
    const pct = loadPct(r.power, r.ratedPowerW);
    if (pct != null && pct > 100) return { label: t('cards.limitAbove'), color: ds.sevCritical };
    if (pct != null && pct > 80) return { label: t('cards.phaseHigh'), color: ds.sevAlarm };
    return null;
  };

  const barColor = (pct: number | null, generation: boolean): string => {
    if (generation) return ds.solar;
    if (pct == null) return ds.border;
    if (pct > 100) return ds.sevCritical;
    if (pct > 80) return ds.sevAlarm;
    return ds.primary;
  };

  const mono = { fontFamily: dsFonts.mono, fontVariantNumeric: 'tabular-nums' as const, whiteSpace: 'nowrap' as const };
  const headCell = { color: ds.inkMuted, fontWeight: 600, fontSize: 12, letterSpacing: '0.02em', borderColor: ds.border };

  const LoadBar: React.FC<{ pct: number | null; generation: boolean }> = ({ pct, generation }) => (
    <Box sx={{ display: 'flex', alignItems: 'center', gap: 1, minWidth: 120 }}>
      <Box sx={{ position: 'relative', flex: 1, height: 6, borderRadius: 3, bgcolor: ds.surfaceRaised, overflow: 'hidden' }}>
        {pct != null && (
          <Box sx={{ position: 'absolute', inset: 0, width: `${Math.min(100, pct)}%`, bgcolor: barColor(pct, generation), borderRadius: 3 }} />
        )}
      </Box>
      <Typography sx={{ ...mono, fontSize: 12, color: ds.inkMuted, width: 38, textAlign: 'right' }}>
        {pct != null ? `${nf(pct)}%` : '—'}
      </Typography>
    </Box>
  );

  // Scroll inside the card instead of clipping (spec §9: tables scroll horizontally in the
  // card on narrow screens); the line name stays pinned while scrolling.
  const sticky = { position: 'sticky' as const, left: 0, zIndex: 1, bgcolor: ds.surface };
  return (
    <Box sx={{ overflowX: 'auto', mx: -0.5 }} tabIndex={0} role="region" aria-label={t('cards.linesAria')}>
    <Table size="small" aria-label={t('cards.linesAria')} sx={{ minWidth: 640 }}>
      <TableHead>
        <TableRow>
          <TableCell sx={{ ...headCell, ...sticky }}>{t('cards.colLine')}</TableCell>
          <TableCell sx={headCell} align="center">{t('cards.colPhase')}</TableCell>
          {/* The lens decides which leads: money mode puts «≈ в час» first, P second. */}
          {moneyFirst && <TableCell sx={headCell} align="right">{t('cards.colPerHour')}</TableCell>}
          <TableCell sx={headCell} align="right">P</TableCell>
          {showMoney && !moneyFirst && <TableCell sx={headCell} align="right">{t('cards.colPerHour')}</TableCell>}
          <TableCell sx={headCell} align="right">I</TableCell>
          <TableCell sx={headCell} align="right">{t('cards.colToday')}</TableCell>
          <TableCell sx={headCell}>{t('cards.colLoad')}</TableCell>
          <TableCell sx={headCell}>{t('cards.colStatus')}</TableCell>
        </TableRow>
      </TableHead>
      <TableBody>
        {rows.map((r) => {
          const st = statusOf(r);
          const pct = loadPct(r.power, r.ratedPowerW);
          return (
            <TableRow
              key={r.key}
              hover
              // A row is not a link element (<a> inside <tbody> is invalid DOM — HEU-32/RES-26):
              // the row navigates by click / Enter and announces itself as a link.
              role="link" tabIndex={0}
              onClick={() => navigate(`/point/${r.key}`)}
              onKeyDown={(e: React.KeyboardEvent) => { if (e.key === 'Enter') navigate(`/point/${r.key}`); }}
              sx={{ textDecoration: 'none', cursor: 'pointer', '& td': { borderColor: ds.border } }}
            >
              <TableCell sx={{ color: r.present ? ds.ink : ds.inkMuted, fontWeight: 500, ...sticky, maxWidth: 180, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }} title={r.name}>{r.name}</TableCell>
              <TableCell align="center" sx={{ color: ds.inkMuted, fontSize: 13, whiteSpace: 'nowrap' }}>{phaseLabel(r.phase)}</TableCell>
              {moneyFirst && moneyCell(r)}
              {/* Export (negative p_act) reads «↑ 471 Вт» in the export colour, like every other figure. */}
              <TableCell align="right" title={r.estimated ? t('cards.balanceEstimate') : undefined} sx={{ ...mono, color: (r.power ?? 0) < 0 ? ds.flowExport : moneyFirst ? ds.inkMuted : ds.ink }}>
                {(r.power ?? 0) < 0 ? '↑ ' : ''}{r.estimated ? '≈ ' : ''}{fmtPower(r.power == null ? null : Math.abs(r.power))}
              </TableCell>
              {showMoney && !moneyFirst && moneyCell(r)}
              <TableCell align="right" sx={{ ...mono, color: ds.inkMuted }}>{fmtCurrent(r.current)}</TableCell>
              {(() => {
                const v = fmtLens.energy(r.energyDay, r.isGeneration, r.isGeneration ? fmtLens.genMoney(r.energyDay) : undefined, r.isGeneration);
                return <TableCell align="right" title={v.sub ?? undefined} sx={{ ...mono, color: v.money ? ds.ink : ds.inkMuted }}>{v.money ? v.main : fmtEnergy(r.energyDay)}</TableCell>;
              })()}
              <TableCell><LoadBar pct={pct} generation={r.isGeneration} /></TableCell>
              <TableCell>
                {st && (
                  <Chip
                    size="small"
                    label={r.present ? st.label : `○ ${st.label}`}
                    sx={{ bgcolor: 'transparent', color: st.color, border: `1px solid ${st.color}`, fontSize: 12, height: 24 }}
                  />
                )}
              </TableCell>
            </TableRow>
          );
        })}

        {showRemainder && (
          <TableRow sx={{ '& td': { borderColor: ds.border, borderTop: `1px solid ${ds.borderStrong}` } }}>
            <TableCell sx={{ color: ds.inkMuted, fontStyle: 'italic', ...sticky }}>{t('cards.remainderRow')}</TableCell>
            <TableCell align="center" sx={{ color: ds.inkMuted }}>—</TableCell>
            <TableCell align="right" sx={{ ...mono, color: ds.inkMuted }}>{fmtPower(remainder)}</TableCell>
            {showMoney && <TableCell />}
            <TableCell /><TableCell /><TableCell /><TableCell />
          </TableRow>
        )}

        {rows.length === 0 && (
          <TableRow>
            <TableCell colSpan={showMoney ? 8 : 7} sx={{ color: ds.inkMuted, textAlign: 'center', py: 3, borderColor: ds.border }}>
              {t('cards.noLines')}
            </TableCell>
          </TableRow>
        )}
      </TableBody>
    </Table>
    </Box>
  );
};
