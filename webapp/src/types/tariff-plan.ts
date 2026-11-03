/**
 * Tariff plan types — mirror of /api/system/tariff-plan-config (BaseResponse, data.list[]).
 * Confirmed structure from root 2026-06-20. POST body = the same full BaseResponse envelope.
 * Supersedes the divergent SimpleTariff* in the old mock/types.
 */

import { TariffPlanType, TariffZoneId, TariffConditionType } from '../constants/enums';

export interface TariffZoneCondition {
  condition_type: TariffConditionType;
  is_active: boolean;
  priority: number;
  /** Shape depends on condition_type (TIME_RANGE: start/end hour/minute; WEEKDAY: weekday_mask; …). */
  params: Record<string, unknown>;
}

export interface TariffZone {
  zone_id: TariffZoneId | number;
  zone_name: string;
  description?: string;
  rate_buying_per_kwh: number;
  /**
   * Feed-in price of the zone (canon 4988e0c7: GET reports it; POST takes an explicit value,
   * without one the hub derives buying × default ratio).
   */
  rate_selling_per_kwh?: number;
  /** Zone's net-metering ratio (canon 4988e0c7; independent of the selling price). */
  net_metering_ratio?: number;
  base_multiplier?: number;
  seasonal_multiplier?: number;
  critical_peak_multiplier?: number;
  is_enabled: boolean;
  is_currently_active?: boolean;
  conditions?: TariffZoneCondition[];
  /** Conditions AND-ed (default) or OR-ed — GET/POST since canon 54fffc83. */
  conditions_and_logic?: boolean;
}

export interface TariffProgressiveTier {
  threshold_kwh: number;
  rate_multiplier: number;
  fixed_rate_per_kwh: number;
  use_multiplier: boolean;
  tier_name: string;
}

export interface TariffProgressiveConfig {
  is_enabled: boolean;
  tiers_count?: number;
  calculation_period?: 'DAILY' | 'MONTHLY' | 'YEARLY';
  reset_monthly?: boolean;
  reset_day?: number;
  thresholds?: TariffProgressiveTier[];
}

export interface TariffFixedPayment {
  payment_type: string;
  payment_name: string;
  amount: number;
  currency: string;
  is_enabled: boolean;
  billing_period?: string;
  billing_day?: number;
}

export interface TariffPlan {
  plan_name: string;
  provider_name?: string;
  description?: string;
  plan_type?: TariffPlanType;
  currency_code: string;
  region_code?: string;
  vat_percent?: number;
  energy_tax_per_kwh?: number;
  grid_usage_fee_per_kwh?: number;
  renewable_levy_per_kwh?: number;
  net_metering_enabled?: boolean;
  net_metering_default_ratio?: number;
  default_selling_rate_per_kwh?: number;
  net_metering_period?: string;
  is_active?: boolean;
  is_default?: boolean;
  default_zone_id?: string | number;
  /** Validity window (unix s; 0/absent = open). The hub keeps pricing with an expired plan. */
  valid_from_timestamp?: number;
  valid_until_timestamp?: number;
  active_zones_count?: number;
  zones: TariffZone[];
  progressive_config?: TariffProgressiveConfig;
  fixed_payments?: TariffFixedPayment[];
  fixed_payments_count?: number;
}

/** data payload of GET/POST /api/system/tariff-plan-config (inside BaseResponse). */
export interface TariffPlanConfigData {
  /**
   * NOT present on the real device (root-confirmed 2026-08-07): there is no settings
   * wrapper — currency/vat/taxes/net_metering all live FLAT inside the plan. The device
   * parser only reads `list` (and takes list[0]). Kept optional for legacy mock fixtures.
   */
  tariff_settings?: Record<string, unknown>;
  list: TariffPlan[];
}
