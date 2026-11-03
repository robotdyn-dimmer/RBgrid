/**
 * PointsTable — the Аналитика «Таблица точек» (spec §4.4.4, review 04 §5.5 / P1-13): one
 * compact row per real point (pseudo units such as the bus voltage excluded) instead of the
 * tile grid. Columns: name · role · P · today · status. Status ONLY on deviation (нет связи /
 * тревога / выше номинала / высокая); a normal row is empty. Offline rows sort last and read
 * «—», never «0 Вт». Row → `/point/:key`. Scrolls horizontally inside the card on phones.
 */
import React from 'react';
import { useNavigate } from 'react-router-dom';
import { Box, Table, TableBody, TableCell, TableHead, TableRow, Typography } from '@mui/material';
import { useLiveSeries } from '../../hooks/useLiveSeries';
import { Sparkline } from './Sparkline';
import { useDsTokens } from '../../theme/useDsTokens';
import { UnitEntityGroup, unitMetricValue } from '../../entities/model';
import { UnitsConfigJoin } from '../../hooks/useUnitsConfig';
import { signedPowerW, fullPower, isMainKey } from '../../entities/siteModel';
import { pointUnits, fullNameOf, roleLabelOf, isGenerationUnit } from '../../pages/overviewData';
import { nf } from '../common/ruFormat';
import { useLensFormat } from '../../economics/MoneyLens';
import { t } from '../../i18n';

export interface PointsTableProps {
  groups: UnitEntityGroup[];
  join: UnitsConfigJoin;
  lost?: boolean;
  /** The input's day energy + exact money (the same figures as «Сегодня» — UX audit T4: the
   *  table showed ≈ 1,01 € next to «Взято 0,00 €»). */
  inputToday?: { wh: number | null; money: number | null };
}

interface Row {
  key: string;
  name: string;
  role: string;
  power: number | null;
  /** W9: `power` is the ×N balance estimate of a single-CT multi-phase point. */
  estimated: boolean;
  todayWh: number | null;
  todayOut: boolean;
  gen: boolean;
  /** Exact money for «today» when the hub has it (the input). */
  todayMoney?: number | null;
  present: boolean;
  status: { label: string; color: string } | null;
  main: boolean;
}

export const PointsTable: React.FC<PointsTableProps> = ({ groups, join, lost, inputToday }) => {
  const ds = useDsTokens();
  const navigate = useNavigate();
  // Trend since the page opened (live frames; no extra history requests to the hub).
  const trend = useLiveSeries(groups, signedPowerW);
  // «Мощность ⇄ Деньги»: P and «Сегодня» follow the lens; the other unit goes to the tooltip.
  const lens = useLensFormat();

  const rows: Row[] = pointUnits(groups).map((g) => {
    const cfg = join.forGroup(g);
    const gen = isGenerationUnit(g, join);
    const { w: p, estimated } = fullPower(g, cfg);
    const rated = cfg?.ratedPowerW;
    const pct = p != null && rated ? (Math.abs(p) / rated) * 100 : null;
    // The input: legacy key ("main") or, on current installs, the MAIN_SUPPLY role.
    const isInput = isMainKey(g.unitKey) || cfg?.role === 'MAIN_SUPPLY';
    let status: Row['status'] = null;
    if (!g.present) status = { label: `○ ${t(g.noChannel ? 'cards.noChannel' : 'cards.offline')}`, color: ds.statusOffline };
    else if (g.attributes?.['alarm'] === true) status = { label: `⚠ ${t('cards.statusAlarm')}`, color: ds.sevAlarm };
    // The CT at its limit outranks «выше номинала»: the reading itself is cut (UI3 live, .139 18.10).
    else if (g.attributes?.clip === true) status = { label: `⚠ ${t('cards.ctClipShort')}`, color: ds.sevAlarm };
    else if (g.attributes?.ct_warn === true) status = { label: t('cards.ctWarnShort'), color: ds.sevWarning };
    else if (!gen && !isInput && pct != null && pct > 100) status = { label: t('cards.aboveRated'), color: ds.sevCritical };
    else if (!gen && !isInput && pct != null && pct > 80) status = { label: `${t('cards.phaseHigh')} · ${nf(pct)}%`, color: ds.sevAlarm };
    return {
      key: g.unitKey,
      name: fullNameOf(g, join),
      role: roleLabelOf(g, join),
      power: p,
      estimated: estimated && p != null && g.present,
      // Generation's «today» is what it gave; everything else — what it took.
      todayWh: isInput && inputToday ? inputToday.wh : unitMetricValue(g, gen ? 'energy_day_out' : 'energy_day_in'),
      todayOut: gen,
      gen,
      todayMoney: isInput && inputToday ? inputToday.money : gen ? lens.genMoney(unitMetricValue(g, 'energy_day_out')) : null,
      present: g.present,
      status,
      main: isInput,
    };
  }).sort((a, b) => {
    if (a.present !== b.present) return a.present ? -1 : 1;
    if (a.main !== b.main) return a.main ? -1 : 1;
    return Math.abs(b.power ?? 0) - Math.abs(a.power ?? 0);
  });

  const num = { fontVariantNumeric: 'tabular-nums' as const, whiteSpace: 'nowrap' as const };
  const headCell = { color: ds.inkMuted, fontWeight: 600, fontSize: 12, letterSpacing: '0.02em', borderColor: ds.border };
  // Phones (< 420): the role column yields its width to P / today (name keeps the context).
  const roleCol = { display: { xs: 'none', sm: 'table-cell' } };
  const sticky = { position: 'sticky' as const, left: 0, zIndex: 1, bgcolor: ds.surface };

  return (
    <Box sx={{ overflowX: 'auto', mx: -0.5, opacity: lost ? 0.6 : 1 }} tabIndex={0} role="region" aria-label={t('cards.pointsAria')}>
      <Table size="small" aria-label={t('cards.pointsAria')} sx={{ minWidth: { xs: 300, sm: 520 } }}>
        <TableHead>
          <TableRow>
            <TableCell sx={{ ...headCell, ...sticky }}>{t('cards.colPoint')}</TableCell>
            <TableCell sx={{ ...headCell, ...roleCol }}>{t('cards.colRole')}</TableCell>
            <TableCell sx={headCell} align="right">{lens.moneyMode ? t('cards.colPerHour') : 'P'}</TableCell>
            <TableCell sx={{ ...headCell, ...roleCol }} title={t('cards.colTrendHint')}>{t('cards.colTrend')}</TableCell>
            <TableCell sx={headCell} align="right">{t('cards.colToday')}</TableCell>
            <TableCell sx={headCell}>{t('cards.colStatus')}</TableCell>
          </TableRow>
        </TableHead>
        <TableBody>
          {rows.map((r) => (
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
              <TableCell sx={{ ...sticky, color: r.present ? ds.ink : ds.inkMuted, fontWeight: 500, maxWidth: { xs: 120, sm: 200 }, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }} title={r.name}>
                {r.name}
              </TableCell>
              <TableCell sx={{ color: ds.inkMuted, fontSize: 13, whiteSpace: 'nowrap', ...roleCol }}>{r.role}</TableCell>
              {(() => {
                const v = lens.power(r.present ? r.power : null, true, r.gen);
                return (
                  <TableCell align="right" title={[v.sub, r.estimated ? t('cards.balanceEstimate') : null].filter(Boolean).join(' · ') || undefined} sx={{ ...num, color: r.present ? (r.power != null && r.power < 0 ? ds.flowExport : ds.ink) : ds.inkMuted }}>
                    {/* one «≈»: money already carries it (GEN-10 «≈ ≈ 114,84 ₽/ч») */}
                    {r.estimated && !v.money ? '≈ ' : ''}{v.main}
                  </TableCell>
                );
              })()}
              <TableCell sx={{ ...roleCol, py: 0 }}>
                <Sparkline values={trend.get(r.key) ?? []} color={r.present ? (r.power != null && r.power < 0 ? ds.flowExport : ds.primary) : ds.statusOffline} />
              </TableCell>
              {(() => {
                const v = lens.energy(r.todayWh, r.todayOut, r.todayMoney, r.gen); // the sun's money is an estimate (≈)
                return (
                  <TableCell align="right" title={v.sub ?? undefined} sx={{ ...num, color: v.money && r.todayOut ? ds.flowExport : ds.inkMuted }}>
                    {v.main}
                  </TableCell>
                );
              })()}
              <TableCell sx={{ whiteSpace: 'nowrap' }}>
                {r.status && <Typography component="span" sx={{ fontSize: 12, fontWeight: 600, color: r.status.color }}>{r.status.label}</Typography>}
              </TableCell>
            </TableRow>
          ))}
          {rows.length === 0 && (
            <TableRow>
              <TableCell colSpan={6} sx={{ color: ds.inkMuted, textAlign: 'center', py: 3, borderColor: ds.border }}>{t('cards.noPoints')}</TableCell>
            </TableRow>
          )}
        </TableBody>
      </Table>
    </Box>
  );
};
