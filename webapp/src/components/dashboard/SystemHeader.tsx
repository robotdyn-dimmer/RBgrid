/**
 * SystemHeader — top-of-dashboard system KPIs (net power / direction / frequency) +
 * connection. Thin adapter mapping normalized telemetry onto the @rbgrid/design-system
 * <SystemSummary> (single brand source).
 */
import React from 'react';
import { SystemSummary, SummaryStat, StatusKind } from '@rbgrid/design-system';
import { NormalizedTelemetry } from '../../entities/model';
import { TelemetryConnState } from '../../hooks/useTelemetryEntities';
import { DeviceCapabilities, isOssBuild } from '../../core/schema/capabilities';
import { classifyTimeQuality, TIME_QUALITY_GLYPH } from '../../core/quality/quality';
import { fmt, fmtPower, fmtEnergy } from '../common/format';

const CONN_KIND: Record<TelemetryConnState, StatusKind> = {
  live: 'live',
  snapshot: 'present',
  connecting: 'dormant',
  error: 'error',
};

/**
 * SystemHeader — the Overview's system summary (spec §4.2 items 1/2/5): main power +
 * direction + frequency, "today" energy/cost, and the time-quality indicator. Gated by
 * schema: cost shows only on a non-OSS build; "today" energy is hidden when tq=0 (the day
 * isn't established yet — spec §5.2), surfaced as an explanatory stat instead of 0.00.
 */
export const SystemHeader: React.FC<{
  data: NormalizedTelemetry;
  conn: TelemetryConnState;
  caps?: DeviceCapabilities;
}> = ({ data, conn, caps }) => {
  const { net_pwr, net_dir, freq, totals, tq, sync_age_s } = data.system;
  const power = totals?.p_act ?? net_pwr;
  const direction = net_dir > 0 ? 'Importing' : net_dir < 0 ? 'Exporting' : 'Balanced';
  const dirColor: SummaryStat['subColor'] =
    net_dir > 0 ? 'warning.main' : net_dir < 0 ? 'success.main' : 'text.secondary';
  const presentCount = data.units.filter((u) => u.present).length;
  const timeSynced = tq === undefined || tq === 1; // undefined (v1 frame) → treat as synced
  const oss = caps ? isOssBuild(caps) : false;

  const stats: SummaryStat[] = [
    { label: 'Power', value: fmtPower(power), sub: direction, subColor: dirColor, emphasis: true },
    { label: 'Frequency', value: fmt(freq, 'Hz', 2) },
    { label: 'Units', value: `${presentCount}/${data.units.length}` },
  ];

  // Time-quality indicator (only when the frame carries tq — v2 / additive-live).
  if (tq !== undefined) {
    const level = classifyTimeQuality(tq);
    const ageTxt = level === 'synced' ? 'synced' : level === 'holdover' ? `holdover ${fmt(sync_age_s, 's', 0)}` : 'not synced';
    stats.push({
      label: 'Time', value: TIME_QUALITY_GLYPH[level], sub: ageTxt,
      subColor: level === 'synced' ? 'success.main' : level === 'holdover' ? 'warning.main' : 'error.main',
    });
  }

  if (totals) {
    if (!timeSynced) {
      // tq=0: the "today" window isn't defined → don't show a misleading number.
      stats.push({ label: 'Today', value: '—', sub: 'time not synced', subColor: 'warning.main' });
    } else {
      stats.push({ label: 'Today in', value: fmtEnergy(totals.e_day_in) });
      if ((totals.e_day_out ?? 0) > 0) {
        stats.push({ label: 'Today out', value: fmtEnergy(totals.e_day_out), sub: 'exported', subColor: 'success.main' });
      }
      if (totals.cost_day != null && !oss) {
        stats.push({ label: 'Cost today', value: fmt(totals.cost_day, undefined, 2) });
      }
    }
  }

  return <SystemSummary stats={stats} connection={CONN_KIND[conn]} connectionLabel={conn} />;
};
