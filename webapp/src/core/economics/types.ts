/**
 * Economics + summary contract types (the billing moat). Forms confirmed by root
 * 2026-06-23 against the firmware handlers — BARE responses, money RAW in `currency`
 * (not cents), energy in kWh, 4-decimal rounding.
 *
 * NOTE the seams root flagged: economics is CUMULATIVE (no from/to window); summary
 * `totals` are windowed but `by_zone` is cumulative. The UI labels these so users
 * aren't misled.
 */

/** The 8 firmware tariff zones (fixed order). */
export const TARIFF_ZONES = [
  'night', 'morning_peak', 'day', 'evening_peak',
  'late_evening', 'weekend', 'holiday', 'critical_peak',
] as const;
export type TariffZoneName = typeof TARIFF_ZONES[number];

export interface ZoneEconomics {
  zone: string;
  cost: number;
  revenue: number;
}

export interface NetMetering {
  period_import_kwh: number;
  period_export_kwh: number;
  credit_balance_kwh: number;
  lifetime_earned_kwh: number;
  lifetime_used_kwh: number;
  last_billable_net_kwh: number;
}

export interface ProgressiveState {
  enabled: boolean;
  cumulative_import_kwh: number;
  tier: number;
  rate_mult: number;
}

/** GET /api/v1/economics?unit=<hex> — cumulative billing state. */
export interface Economics {
  v: number;
  unit: string;
  currency: string;
  zones: ZoneEconomics[];
  net_metering: NetMetering;
  progressive: ProgressiveState;
}

export interface SummaryTotals {
  records: number;
  energy_in_kwh: number;
  energy_out_kwh: number;
  net_kwh: number;
  cost: number;
  revenue: number;
  net_cost: number;
}

/** GET /api/v1/summary?unit=&period=&from=&to= — windowed totals + (cumulative) by-zone. */
export interface Summary {
  v: number;
  unit: string;
  period: string;
  currency: string;
  totals: SummaryTotals;
  by_zone: ZoneEconomics[];
}

export type SummaryPeriod = 'hour' | 'day' | 'week' | 'month' | 'year';
