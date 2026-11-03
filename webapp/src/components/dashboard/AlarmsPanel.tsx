/**
 * AlarmsPanel — deviations derived from telemetry (spec §4.4.2 «Активные тревоги», reused
 * on Предприятие as «Требует внимания»). A dedicated events/log endpoint is a contract gap,
 * so the panel lists what is deviating NOW: units flagged alarming («серьёзно») and points
 * without connection («нет связи»). Pseudo units (bus voltage) are never listed.
 *
 * Status-strip behaviour (review 04 §5.1 / P1-9): with nothing deviating the panel collapses
 * to ONE line («✓ Тревог нет · N точек на связи») instead of a half-screen empty card.
 * Severity is always a word, never colour alone (§9).
 */
import React from 'react';
import { Box, Stack, Typography } from '@mui/material';
import { Link as RouterLink } from 'react-router-dom';
import { Card } from '@rbgrid/design-system';
import { useDsTokens } from '../../theme/useDsTokens';
import { UnitEntityGroup, unitMetricValue } from '../../entities/model';
import { isPseudoKey } from '../../entities/siteModel';
import { pwW } from '../common/ruFormat';
import { t } from '../../i18n';

export interface AlarmsPanelProps {
  groups: UnitEntityGroup[];
  title?: string;
  /** Display-name resolver (configured short name → name → slug); defaults to the group name. */
  nameOf?: (g: UnitEntityGroup) => string;
  /** Line nominal (W) by point — a line above it needs attention too (INP-R10: 148 % was not listed). */
  ratedOf?: (g: UnitEntityGroup) => number | undefined;
}

function isAlarm(g: UnitEntityGroup): boolean {
  return g.attributes?.['alarm'] === true;
}

/** «N точек на связи» — plural forms live in the phrase file (cards.pointsOnline_*). */
const pointsOnline = (n: number): string => t('cards.pointsOnline', { count: n });

export const AlarmsPanel: React.FC<AlarmsPanelProps> = ({ groups, title: titleProp, nameOf, ratedOf }) => {
  const title = titleProp ?? t('cards.alarmsTitle');
  const ds = useDsTokens();
  // A point on a channel the module lacks isn't watched: nothing measures it (UI3 in place of UI7).
  const points = groups.filter((g) => !isPseudoKey(g.unitKey) && !g.noChannel);
  const alarms = points.filter((g) => g.present && isAlarm(g));
  const offline = points.filter((g) => !g.present);
  const online = points.length - offline.length;
  const name = (g: UnitEntityGroup) => (nameOf ? nameOf(g) : g.name);
  const pctOf = (g: UnitEntityGroup): number | null => {
    const r = ratedOf?.(g);
    const p = unitMetricValue(g, 'power');
    return r && r > 0 && p != null && p > 0 ? (p / r) * 100 : null;
  };
  // A clipping point is listed once, as «срез»: its power is under-counted, so «выше номинала» adds nothing.
  const over = points.filter((g) => g.present && !isAlarm(g) && g.attributes?.clip !== true && (pctOf(g) ?? 0) > 100);
  // Present but holding an old value (hub STALE / age_s) — the module stopped answering (SCADA-R04).
  const stale = points.filter((g) => g.present && g.stale);
  // The CT at its limit (hub clip / ct_warn, BASIC 18.10): current above it is cut off and the
  // metering under-counts silently — listed per point, so several channels read at a glance.
  const clip = points.filter((g) => g.present && !isAlarm(g) && g.attributes?.clip === true);
  const ctWarn = points.filter((g) => g.present && !isAlarm(g) && g.attributes?.clip !== true && g.attributes?.ct_warn === true);
  const count = alarms.length + offline.length + over.length + stale.length + clip.length + ctWarn.length;

  if (count === 0) {
    return (
      <Card>
        <Stack direction="row" spacing={1} alignItems="center" flexWrap="wrap" useFlexGap>
          <Typography sx={{ fontWeight: 700, fontSize: 15, mr: 0.5 }}>{title}</Typography>
          <Box sx={{ color: ds.statusOnline, fontSize: 15, lineHeight: 1 }} aria-hidden>✓</Box>
          <Typography sx={{ color: ds.inkMuted, fontSize: 14 }}>
            {t('cards.alarmsNone')} · {pointsOnline(online)}
          </Typography>
        </Stack>
      </Card>
    );
  }

  const loadPct = (g: UnitEntityGroup): string => (typeof g.attributes?.load_pct === 'number' ? String(Math.round(g.attributes.load_pct as number)) : '—');
  const Row: React.FC<{ g: UnitEntityGroup; kind: 'alarm' | 'offline' | 'over' | 'stale' | 'clip' | 'ctWarn' }> = ({ g, kind }) => {
    const color = kind === 'offline' ? ds.statusOffline : kind === 'ctWarn' ? ds.sevWarning : ds.sevAlarm;
    return (
      <Box
        component={RouterLink}
        to={`/point/${g.unitKey}`}
        sx={{
          display: 'flex', alignItems: 'center', gap: 1.5,
          p: 1, borderRadius: 1, textDecoration: 'none',
          border: `1px solid ${kind === 'offline' ? ds.border : kind === 'ctWarn' ? ds.sevWarning : ds.sevAlarm}`,
          bgcolor: kind === 'offline' ? 'transparent' : kind === 'ctWarn' ? ds.sevWarningSoft : ds.sevAlarmSoft,
        }}
      >
        <Box sx={{ color, fontSize: 16, lineHeight: 1 }} aria-hidden>{kind === 'offline' ? '○' : '⚠'}</Box>
        <Box sx={{ flex: 1, minWidth: 0 }}>
          <Typography sx={{ fontWeight: 600, fontSize: 14, color: ds.ink }} noWrap>{name(g)}</Typography>
          <Typography sx={{ fontSize: 12, color: ds.inkMuted }}>
            {kind === 'clip' ? t('cards.ctClipRow', { pct: loadPct(g) })
              : kind === 'ctWarn' ? t('cards.ctWarnRow', { pct: loadPct(g) })
              : kind === 'alarm' ? `${t('cards.statusAlarm')} · ${pwW(unitMetricValue(g, 'power'))}`
              : kind === 'over' ? t('cards.overRated', { pct: Math.round(pctOf(g) ?? 0), power: pwW(unitMetricValue(g, 'power')) })
                : kind === 'stale' ? t('cards.staleRow', { s: Math.round(g.ageS ?? 6) })
                : t('cards.sensorNotResponding')}
          </Typography>
        </Box>
        <Typography sx={{ fontSize: 12, color, fontWeight: 600, whiteSpace: 'nowrap' }}>
          {kind === 'clip' ? t('cards.ctClipShort') : kind === 'ctWarn' ? t('cards.ctWarnShort') : kind === 'alarm' ? t('cards.serious') : kind === 'over' ? t('cards.aboveRated') : kind === 'stale' ? t('cards.staleShort') : t('cards.offline')}
        </Typography>
      </Box>
    );
  };

  return (
    <Card>
      <Stack spacing={1.25} sx={{ py: 0.5 }}>
        <Stack direction="row" alignItems="center" justifyContent="space-between">
          <Typography sx={{ fontWeight: 700, fontSize: 15 }}>{title}</Typography>
          <Typography sx={{ fontSize: 12, fontWeight: 600, color: alarms.length ? ds.sevAlarm : ds.inkMuted }}>{count}</Typography>
        </Stack>
        <Stack spacing={1}>
          {alarms.map((g) => <Row key={g.unitKey} g={g} kind="alarm" />)}
          {clip.map((g) => <Row key={`c-${g.unitKey}`} g={g} kind="clip" />)}
          {over.map((g) => <Row key={`o-${g.unitKey}`} g={g} kind="over" />)}
          {ctWarn.map((g) => <Row key={`w-${g.unitKey}`} g={g} kind="ctWarn" />)}
          {stale.map((g) => <Row key={`s-${g.unitKey}`} g={g} kind="stale" />)}
          {offline.map((g) => <Row key={g.unitKey} g={g} kind="offline" />)}
        </Stack>
        <Typography sx={{ fontSize: 12, color: ds.inkMuted }}>
          {/* «Тревог нет» next to silent points read as «всё хорошо» (HOME-06) — say on-line of total. */}
          {t('cards.onlineOfTotal', { n: online, m: points.length })}
        </Typography>
      </Stack>
    </Card>
  );
};
