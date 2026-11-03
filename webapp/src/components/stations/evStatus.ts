/**
 * evStatus — derive an EV-charging-station's status from measured power (spec §4.3 U3).
 *
 * TRUTH-OF-DATA (U3): RBGrid measures LINE POWER, not connector state. The status here is
 * DERIVED from power + session timing, never a controller reading. Its tooltip must always
 * read «определено по мощности». Without station-controller integration (OCPP/Modbus,
 * phase 2) there is NO «ограничено N%» and NO «Ошибка» — callers must not render those.
 */
import { EvConfig } from '../../types/config';
import { t } from '../../i18n';

export type StationStatus = 'charging' | 'waiting' | 'free' | 'nodata';

export interface EvThresholds {
  /** P above which a line counts as actively charging (W). */
  chargeThresholdW: number;
  /** Minutes a station stays «Ожидает» after a session before «Свободна». */
  idleTimeoutMin: number;
}

/** UI defaults when `ev` is unset / 0 (spec §4.3, G7). */
export const EV_DEFAULTS: EvThresholds = { chargeThresholdW: 500, idleTimeoutMin: 30 };

/** Resolve thresholds from the EV config, falling back to the UI defaults on 0/absent. */
export function evThresholds(ev?: EvConfig): EvThresholds {
  return {
    chargeThresholdW: ev && ev.charge_threshold_w > 0 ? ev.charge_threshold_w : EV_DEFAULTS.chargeThresholdW,
    idleTimeoutMin: ev && ev.idle_timeout_min > 0 ? ev.idle_timeout_min : EV_DEFAULTS.idleTimeoutMin,
  };
}

/** One station's thresholds: its own (unit fields, root 56be1d45) over the hub's (system.ev). */
export function stationThresholds(hub: EvThresholds, unit?: { chargeThresholdW?: number; idleTimeoutMin?: number }): EvThresholds {
  return {
    chargeThresholdW: unit?.chargeThresholdW && unit.chargeThresholdW > 0 ? unit.chargeThresholdW : hub.chargeThresholdW,
    idleTimeoutMin: unit?.idleTimeoutMin && unit.idleTimeoutMin > 0 ? unit.idleTimeoutMin : hub.idleTimeoutMin,
  };
}

/**
 * Derive station status (spec §4.3): P > threshold → charging; P below but a session began
 * less than idleTimeout ago → waiting; otherwise free. A unit that isn't present → nodata.
 */
export function deriveStationStatus(
  powerW: number | null,
  present: boolean,
  sessionStartMs: number | null,
  ev: EvThresholds = EV_DEFAULTS,
): StationStatus {
  if (!present || powerW == null) return 'nodata';
  if (Math.abs(powerW) > ev.chargeThresholdW) return 'charging';
  if (sessionStartMs != null) {
    const ageMin = (Date.now() - sessionStartMs) / 60000;
    if (ageMin >= 0 && ageMin < ev.idleTimeoutMin) return 'waiting';
  }
  return 'free';
}

/** Phrase keys per status (resolved at access time — the language can switch at runtime). */
const STATUS_KEY: Record<StationStatus, string> = {
  charging: 'stations.statusCharging',
  waiting: 'stations.statusWaiting',
  free: 'stations.statusFree',
  nodata: 'stations.statusNodata',
};
const statusEntry = (s: StationStatus): { label: string } => ({ get label() { return t(STATUS_KEY[s]); } });

export const STATION_STATUS: Record<StationStatus, { label: string }> = {
  charging: statusEntry('charging'),
  waiting: statusEntry('waiting'),
  free: statusEntry('free'),
  nodata: statusEntry('nodata'),
};

/** Tooltip shown on every derived status — makes the data-truth explicit (U3). */
export const stationStatusTooltip = (): string => t('stations.statusTooltip');

