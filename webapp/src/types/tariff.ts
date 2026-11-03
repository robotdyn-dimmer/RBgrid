/**
 * Tariff and billing types for RBGrid WebApp
 * These types define the structure of tariff plans, zones, and billing calculations
 */

/**
 * Tariff plan types
 */
export type TariffPlanType = 'FLAT' | 'TWO_ZONE' | 'THREE_ZONE' | 'PROGRESSIVE';

/**
 * Tariff calculation methods
 */
export type TariffCalculationMethod = 'FLAT' | 'PROGRESSIVE' | 'TIME_OF_USE';

/**
 * Optimization priority
 */
export type OptimizationPriority = 'COST' | 'EFFICIENCY' | 'ENVIRONMENTAL';

/**
 * Net metering period
 */
export type NetMeteringPeriod = 'DAILY' | 'MONTHLY' | 'YEARLY';

/**
 * Calculation period for progressive tariffs
 */
export type CalculationPeriod = 'DAILY' | 'MONTHLY' | 'YEARLY';

/**
 * Tariff condition types
 */
export type TariffConditionType = 'TIME_RANGE' | 'SEASON' | 'WEEKDAY' | 'MONTHLY_USAGE';

/**
 * Tariff condition parameters
 */
export interface TariffConditionParams {
  /** Start hour (0-23) for TIME_RANGE */
  start_hour?: number;
  /** Start minute (0-59) for TIME_RANGE */
  start_minute?: number;
  /** End hour (0-23) for TIME_RANGE */
  end_hour?: number;
  /** End minute (0-59) for TIME_RANGE */
  end_minute?: number;
  /** Time range spans midnight */
  spans_midnight?: boolean;
  /** Season months for SEASON condition */
  months?: number[];
  /** Weekdays (0-6, 0=Sunday) for WEEKDAY condition */
  weekdays?: number[];
  /** Usage threshold in kWh for MONTHLY_USAGE */
  usage_threshold?: number;
  /** Additional parameters */
  [key: string]: any;
}

/**
 * Tariff condition
 */
export interface TariffCondition {
  /** Condition type */
  condition_type: TariffConditionType;
  /** Condition parameters */
  params: TariffConditionParams;
  /** Condition active status */
  is_active: boolean;
  /** Condition priority (higher = more priority) */
  priority: number;
}

/**
 * Tariff zone configuration
 */
export interface TariffZone {
  /** Zone identifier */
  zone_id: string;
  /** Zone display name */
  zone_name: string;
  /** Zone description */
  description: string;
  /** Rate for buying electricity (per kWh) */
  rate_buying_per_kwh: number;
  /** Rate for selling electricity (per kWh) */
  rate_selling_per_kwh: number;
  /** Net metering ratio (0-1) */
  net_metering_ratio: number;
  /** Base rate multiplier */
  base_multiplier: number;
  /** Seasonal adjustment multiplier */
  seasonal_multiplier: number;
  /** Critical peak pricing multiplier */
  critical_peak_multiplier: number;
  /** Zone activation conditions */
  conditions: TariffCondition[];
  /** Zone enabled status */
  is_enabled: boolean;
  /** Zone currently active */
  is_currently_active: boolean;
  /** Last activation timestamp */
  last_activation_timestamp: number;
  /** Number of activations today */
  activations_count_today: number;
}

/**
 * Progressive tariff configuration
 */
export interface ProgressiveConfig {
  /** Progressive pricing enabled */
  is_enabled: boolean;
  /** Number of tiers */
  tiers_count: number;
  /** Usage thresholds for each tier (kWh) */
  thresholds: number[];
  /** Calculation period */
  calculation_period: CalculationPeriod;
  /** Reset usage monthly */
  reset_monthly: boolean;
  /** Day of month to reset (1-31) */
  reset_day: number;
}

/**
 * Fixed payment types
 */
export type FixedPaymentType = 'MONTHLY_FEE' | 'CONNECTION_FEE' | 'METER_RENTAL' | 'OTHER';

/**
 * Fixed payment configuration
 */
export interface FixedPayment {
  /** Payment ID */
  payment_id: string;
  /** Payment type */
  type: FixedPaymentType;
  /** Payment name */
  name: string;
  /** Payment amount */
  amount: number;
  /** Billing frequency */
  frequency: 'MONTHLY' | 'QUARTERLY' | 'YEARLY' | 'ONE_TIME';
  /** Payment enabled */
  is_enabled: boolean;
  /** VAT included in amount */
  vat_included: boolean;
}

/**
 * Tariff plan configuration
 */
export interface TariffPlan {
  /** Plan name */
  plan_name: string;
  /** Energy provider name */
  provider_name: string;
  /** Plan description */
  description: string;
  /** Plan type */
  plan_type: TariffPlanType;
  /** Number of active zones */
  active_zones_count: number;
  /** Default zone ID */
  default_zone_id: string;
  /** Tariff zones */
  zones: TariffZone[];
  /** Progressive pricing configuration */
  progressive_config: ProgressiveConfig;
  /** Fixed payments */
  fixed_payments: FixedPayment[];
  /** Fixed payments count */
  fixed_payments_count: number;
  /** Currency code (ISO 4217) */
  currency_code: string;
  /** Region code */
  region_code: string;
  /** VAT percentage */
  vat_percent: number;
  /** Energy tax per kWh */
  energy_tax_per_kwh: number;
  /** Grid usage fee per kWh */
  grid_usage_fee_per_kwh: number;
  /** Renewable energy levy per kWh */
  renewable_levy_per_kwh: number;
  /** Net metering enabled */
  net_metering_enabled: boolean;
  /** Default net metering ratio */
  net_metering_default_ratio: number;
  /** Net metering calculation period */
  net_metering_period: NetMeteringPeriod;
  /** Carry over unused credits */
  carry_over_credits: boolean;
  /** Maximum credit carry over in kWh */
  max_credit_carry_over_kwh: number;
  /** Supported features bitmask */
  supported_features: number;
  /** Calculation method */
  calculation_method: TariffCalculationMethod;
  /** Optimization priority */
  optimization_priority: OptimizationPriority;
  /** Enable load shifting recommendations */
  enable_load_shifting_recommendations: boolean;
  /** Load shifting threshold percentage */
  load_shifting_threshold_percent: number;
  /** Plan valid from timestamp */
  valid_from_timestamp: number;
  /** Plan valid until timestamp */
  valid_until_timestamp: number;
  /** Plan active status */
  is_active: boolean;
  /** Default plan flag */
  is_default: boolean;
  /** Plan version number */
  plan_version: number;
  /** Creation timestamp */
  created_timestamp: number;
  /** Last modification timestamp */
  last_modified_timestamp: number;
}

/**
 * Tariff configuration data
 */
export interface TariffConfigData {
  /** List of tariff plans */
  list: TariffPlan[];
}
