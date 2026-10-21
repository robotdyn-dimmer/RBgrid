/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbhome_tariff.h
 * @brief Tariff engine: plans, time-of-use zones, cost/revenue accounting, net metering and
 *        progressive (block-tier) pricing.
 *
 * The tariff engine turns measured energy into money. A single active plan
 * (::g_current_tariff_plan) defines up to ::RBPOWER_TARIFF_ZONE_COUNT time-of-use zones with
 * buying (import) and selling (feed-in) rates, per-kWh fees, VAT, an optional progressive tier
 * table and net-metering rules. Each finalized period is costed with the zone active at that
 * time (rbpower_update_economics_direct(), rbpower_tariff_cost_injected_period()).
 *
 * Units: all money is in the plan's configured currency ("currency units"; see
 * rbpower_tariff_currency()). Currency is a display label only — no conversion is done.
 * Rates are currency units per kWh. Energy is kWh unless a field says otherwise.
 *
 * Scope: several structures in this header are data models kept for a future calendar /
 * optimization layer and are not instantiated by the current firmware. Their @brief says so.
 * A number of functions are declared but have no definition in this firmware; their
 * documentation says "Not implemented". Do not call them.
 *
 * Thread safety: the engine keeps module-level state (active plan, zone cache, per-unit cost and
 * net-metering tables) without locks. Call it from the statistics/processing task or from the
 * diagnostic console; concurrent writers from other tasks are not supported.
 */
#ifndef RBHOME_TARIFF_H
#define RBHOME_TARIFF_H

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "rbpower_constants.h"
#include "rbpower_enums.h"
#include "rbhome_types.h"

//=============================================================================
// TARIFF SYSTEM CONSTANTS
//=============================================================================

// Maximum values
#define RBPOWER_TARIFF_PROGRESSIVE_TIERS 4     ///< Maximum number of progressive (block) tiers
#define RBPOWER_TARIFF_HOLIDAYS_COUNT 32       ///< Maximum number of yearly-recurring holidays
#define RBPOWER_TARIFF_SEASONS_COUNT 4         ///< Maximum number of seasons
#define RBPOWER_TARIFF_CRITICAL_PEAKS_COUNT 16 ///< Maximum number of critical-peak events per year
#define RBPOWER_TARIFF_NAME_LENGTH 32          ///< Name buffer size (bytes, including NUL)
#define RBPOWER_TARIFF_DESCRIPTION_LENGTH 64   ///< Description buffer size (bytes, including NUL)
#define RBPOWER_TARIFF_CURRENCY_LENGTH 4       ///< Currency code buffer size (ISO 4217 code + NUL)
#define RBPOWER_TARIFF_REGION_LENGTH 8         ///< Region code buffer size (bytes, including NUL)
// #define RBPOWER_TARIFF_ZONE_COUNT           8       // Maximum tariff zones

// Thresholds and limits
#define RBPOWER_TARIFF_MIN_COST_THRESHOLD 0.001f ///< Minimum cost taken into account (currency units)
#define RBPOWER_TARIFF_MAX_MULTIPLIER 10.0f      ///< Maximum allowed tariff multiplier (dimensionless)
#define RBPOWER_TARIFF_DEFAULT_NET_RATIO 0.7f    ///< Default net-metering credit ratio (0.0-1.0)

//=============================================================================
// TARIFF SYSTEM ENUMERATIONS
//=============================================================================

/**
 * @brief Tariff (time-of-use) zone types. Also used as the index into zone arrays.
 */
typedef enum
{
    RBPOWER_TARIFF_ZONE_NIGHT = 0,     ///< Night tariff (usually cheapest)
    RBPOWER_TARIFF_ZONE_MORNING_PEAK,  ///< Morning peak
    RBPOWER_TARIFF_ZONE_DAY,           ///< Day tariff
    RBPOWER_TARIFF_ZONE_EVENING_PEAK,  ///< Evening peak (usually most expensive)
    RBPOWER_TARIFF_ZONE_LATE_EVENING,  ///< Late evening
    RBPOWER_TARIFF_ZONE_WEEKEND,       ///< Weekend day
    RBPOWER_TARIFF_ZONE_HOLIDAY,       ///< Holiday
    RBPOWER_TARIFF_ZONE_CRITICAL_PEAK, ///< Critical peak (emergency)
    RBPOWER_TARIFF_ZONE_COUNT = 8      ///< Number of zone types (array size, not a zone)
} rbpower_tariff_zone_type_t;

/**
 * @brief Text names of tariff zone types.
 *
 * Indexed by ::rbpower_tariff_zone_type_t. These strings appear in logs and API output; do not
 * change them.
 */
extern const char *const rbpower_tariff_zone_type_text[RBPOWER_TARIFF_ZONE_COUNT];

/**
 * @brief Tariff plan types
 */
typedef enum
{
    RBPOWER_TARIFF_PLAN_SIMPLE = 0,  ///< Simple single-rate tariff
    RBPOWER_TARIFF_PLAN_TWO_ZONE,    ///< Two-zone (day/night)
    RBPOWER_TARIFF_PLAN_THREE_ZONE,  ///< Three-zone
    RBPOWER_TARIFF_PLAN_MULTI_ZONE,  ///< Multi-zone (up to 8 zones)
    RBPOWER_TARIFF_PLAN_PROGRESSIVE, ///< Progressive scale
    RBPOWER_TARIFF_PLAN_SEASONAL,    ///< Seasonal adjustments
    RBPOWER_TARIFF_PLAN_DYNAMIC,     ///< Dynamic (critical peaks)
    RBPOWER_TARIFF_PLAN_CUSTOM       ///< Custom plan
} rbpower_tariff_plan_type_t;

/**
 * @brief Text names of tariff plan types, indexed by ::rbpower_tariff_plan_type_t.
 */
extern const char *const rbpower_tariff_plan_type_text[RBPOWER_TARIFF_PLAN_CUSTOM + 1];

/**
 * @brief Condition types that decide when a tariff zone applies.
 */
typedef enum
{
    RBPOWER_TARIFF_CONDITION_TIME_RANGE = 0, ///< Time range (hh:mm - hh:mm)
    RBPOWER_TARIFF_CONDITION_WEEKDAY,        ///< Weekday
    RBPOWER_TARIFF_CONDITION_WEEKEND,        ///< Weekend
    RBPOWER_TARIFF_CONDITION_HOLIDAY,        ///< Holiday
    RBPOWER_TARIFF_CONDITION_SEASON,         ///< Season
    RBPOWER_TARIFF_CONDITION_MONTH,          ///< Specific month
    RBPOWER_TARIFF_CONDITION_DATE_RANGE,     ///< Date range
    RBPOWER_TARIFF_CONDITION_SPECIAL_EVENT   ///< Special event
} rbpower_tariff_condition_type_t;

/**
 * @brief Text names of tariff condition types, indexed by ::rbpower_tariff_condition_type_t.
 */
extern const char *const rbpower_tariff_condition_type_text[RBPOWER_TARIFF_CONDITION_SPECIAL_EVENT + 1];

/**
 * @brief Tariff calculation methods (informational; stored in the plan).
 */
typedef enum
{
    RBPOWER_TARIFF_CALC_FLAT = 0,     ///< Fixed rate per kWh
    RBPOWER_TARIFF_CALC_PROGRESSIVE,  ///< Progressive scale
    RBPOWER_TARIFF_CALC_REGRESSIVE,   ///< Regressive scale (volume discounts)
    RBPOWER_TARIFF_CALC_DEMAND_BASED, ///< Peak demand based
    RBPOWER_TARIFF_CALC_TIME_OF_USE,  ///< Time-of-use
    RBPOWER_TARIFF_CALC_REAL_TIME     ///< Real-time
} rbpower_tariff_calculation_method_t;

/**
 * @brief Fixed payment types
 */
typedef enum
{
    RBPOWER_TARIFF_FIXED_MONTHLY = 0,    ///< Monthly subscription fee
    RBPOWER_TARIFF_FIXED_DAILY,          ///< Daily fee
    RBPOWER_TARIFF_FIXED_POWER_BASED,    ///< Installed capacity based
    RBPOWER_TARIFF_FIXED_CONNECTION,     ///< Connection fee
    RBPOWER_TARIFF_FIXED_GRID_USAGE,     ///< Grid usage fee
    RBPOWER_TARIFF_FIXED_RENEWABLE_LEVY, ///< Renewable energy levy
    RBPOWER_TARIFF_FIXED_TAX,            ///< Taxes
    RBPOWER_TARIFF_FIXED_CUSTOM          ///< Custom fee
} rbpower_tariff_fixed_type_t;

/**
 * @brief Billing direction of a unit.
 */
typedef enum
{
    RBPOWER_TARIFF_DIRECTION_CONSUMPTION = 0, ///< Consumption (energy purchase)
    RBPOWER_TARIFF_DIRECTION_SUPPLY,          ///< Supply (energy sale)
    RBPOWER_TARIFF_DIRECTION_NET,             ///< Net metering
    RBPOWER_TARIFF_DIRECTION_BIDIRECTIONAL    ///< Bidirectional metering
} rbpower_tariff_direction_t;

/**
 * @brief Result status of a tariff calculation.
 */
typedef enum
{
    RBPOWER_TARIFF_STATUS_OK = 0,            ///< Calculation successful
    RBPOWER_TARIFF_STATUS_NO_PLAN,           ///< Tariff plan not configured
    RBPOWER_TARIFF_STATUS_INVALID_TIME,      ///< Invalid time
    RBPOWER_TARIFF_STATUS_ZONE_NOT_FOUND,    ///< Tariff zone not found
    RBPOWER_TARIFF_STATUS_CALCULATION_ERROR, ///< Calculation error
    RBPOWER_TARIFF_STATUS_INSUFFICIENT_DATA, ///< Insufficient data
    RBPOWER_TARIFF_STATUS_DISABLED           ///< Tariff disabled
} rbpower_tariff_status_t;

/**
 * @brief Optimization priorities (informational; no optimizer acts on them yet).
 */
typedef enum
{
    RBPOWER_TARIFF_PRIORITY_COST = 0,       ///< Cost minimization
    RBPOWER_TARIFF_PRIORITY_COMFORT,        ///< Usage comfort
    RBPOWER_TARIFF_PRIORITY_GRID_STABILITY, ///< Grid stability
    RBPOWER_TARIFF_PRIORITY_RENEWABLE,      ///< Renewable maximization
    RBPOWER_TARIFF_PRIORITY_BALANCED        ///< Balanced approach
} rbpower_tariff_optimization_priority_t;

/**
 * @brief Device usage profiles for per-unit tariff tracking (informational).
 */
typedef enum
{
    RBPOWER_DEVICE_PROFILE_ALWAYS_ON = 0,   ///< Always-on devices (refrigerator)
    RBPOWER_DEVICE_PROFILE_SCHEDULED,       ///< Scheduled (heating)
    RBPOWER_DEVICE_PROFILE_ON_DEMAND,       ///< On-demand (kettle)
    RBPOWER_DEVICE_PROFILE_DEFERRABLE,      ///< Deferrable (washing machine)
    RBPOWER_DEVICE_PROFILE_STORAGE,         ///< Storage (batteries)
    RBPOWER_DEVICE_PROFILE_GENERATION,      ///< Local generation (for example a PV inverter); informational only
    RBPOWER_DEVICE_PROFILE_HVAC,            ///< Heating/air conditioning
    RBPOWER_DEVICE_PROFILE_EV_CHARGING,     ///< EV charging
    RBPOWER_DEVICE_PROFILE_SMART_APPLIANCE, ///< Smart appliances
    RBPOWER_DEVICE_PROFILE_CUSTOM           ///< Custom profile
} rbpower_device_profile_type_t;

//=============================================================================
// TARIFF FEATURE BIT MASKS
//=============================================================================

/**
 * @name Tariff feature bits
 * Bits of rbpower_tariff_plan_t::supported_features. The mask is stored and reported through the
 * API; the costing path does not gate on it (net metering and progressive pricing are switched by
 * their own plan fields).
 * @{
 */
#define RBPOWER_TARIFF_FEATURE_NONE 0x00000000                ///< No features
#define RBPOWER_TARIFF_FEATURE_BASIC_BILLING (1 << 0)     ///< Basic billing
#define RBPOWER_TARIFF_FEATURE_TIME_OF_USE (1 << 1)       ///< Multi-zone tariffs
#define RBPOWER_TARIFF_FEATURE_PROGRESSIVE_RATES (1 << 2) ///< Progressive scale
#define RBPOWER_TARIFF_FEATURE_NET_METERING (1 << 3)      ///< Energy credit
#define RBPOWER_TARIFF_FEATURE_CRITICAL_PEAKS (1 << 4)    ///< Critical peaks
#define RBPOWER_TARIFF_FEATURE_SEASONAL_RATES (1 << 5)    ///< Seasonal tariffs
#define RBPOWER_TARIFF_FEATURE_DEVICE_ANALYTICS (1 << 6)  ///< Device analytics
#define RBPOWER_TARIFF_FEATURE_LOAD_OPTIMIZATION (1 << 7) ///< Load optimization
#define RBPOWER_TARIFF_FEATURE_DEMAND_CHARGES (1 << 8)    ///< Demand charges
#define RBPOWER_TARIFF_FEATURE_CARBON_TRACKING (1 << 9)   ///< CO2 tracking
#define RBPOWER_TARIFF_FEATURE_FORECASTING (1 << 10)      ///< Forecasting
#define RBPOWER_TARIFF_FEATURE_EXPORT_DATA (1 << 11)      ///< Data export
/** @} */

/**
 * @name Preset feature combinations
 * @{
 */
#define RBPOWER_TARIFF_FEATURES_MINIMAL (RBPOWER_TARIFF_FEATURE_BASIC_BILLING) ///< Basic billing only
/** @brief Basic billing + time-of-use + net metering (the default for built-in plans). */
#define RBPOWER_TARIFF_FEATURES_STANDARD (RBPOWER_TARIFF_FEATURES_MINIMAL |    \
                                          RBPOWER_TARIFF_FEATURE_TIME_OF_USE | \
                                          RBPOWER_TARIFF_FEATURE_NET_METERING)
/** @brief STANDARD + progressive rates, critical peaks and device analytics. */
#define RBPOWER_TARIFF_FEATURES_ADVANCED (RBPOWER_TARIFF_FEATURES_STANDARD |         \
                                          RBPOWER_TARIFF_FEATURE_PROGRESSIVE_RATES | \
                                          RBPOWER_TARIFF_FEATURE_CRITICAL_PEAKS |    \
                                          RBPOWER_TARIFF_FEATURE_DEVICE_ANALYTICS)
/** @brief ADVANCED + seasonal rates, load optimization, demand charges and forecasting. */
#define RBPOWER_TARIFF_FEATURES_COMPLETE (RBPOWER_TARIFF_FEATURES_ADVANCED |         \
                                          RBPOWER_TARIFF_FEATURE_SEASONAL_RATES |    \
                                          RBPOWER_TARIFF_FEATURE_LOAD_OPTIMIZATION | \
                                          RBPOWER_TARIFF_FEATURE_DEMAND_CHARGES |    \
                                          RBPOWER_TARIFF_FEATURE_FORECASTING)
/** @} */

//=============================================================================
// FEATURE CHECK MACROS
//=============================================================================

/** @brief Evaluate to true if bit(s) @p feature are set in @p features. */
#define RBPOWER_TARIFF_HAS_FEATURE(features, feature) \
    (((features) & (feature)) != 0)

/** @brief Set bit(s) @p feature in the lvalue @p features. */
#define RBPOWER_TARIFF_ENABLE_FEATURE(features, feature) \
    ((features) |= (feature))

/** @brief Clear bit(s) @p feature in the lvalue @p features. */
#define RBPOWER_TARIFF_DISABLE_FEATURE(features, feature) \
    ((features) &= ~(feature))

/** @brief Evaluate to true if @p zone is a valid ::rbpower_tariff_zone_type_t index. */
#define RBPOWER_TARIFF_ZONE_IS_VALID(zone) \
    ((zone) >= 0 && (zone) < RBPOWER_TARIFF_ZONE_COUNT)

/** @brief Evaluate to true if @p mult is within [0, ::RBPOWER_TARIFF_MAX_MULTIPLIER]. */
#define RBPOWER_TARIFF_MULTIPLIER_IS_VALID(mult) \
    ((mult) >= 0.0f && (mult) <= RBPOWER_TARIFF_MAX_MULTIPLIER)

//=============================================================================
// BASIC TARIFF ZONE STRUCTURES
//=============================================================================

/**
 * @brief Time condition for tariff zone
 */
typedef struct
{
    rbpower_tariff_condition_type_t condition_type; ///< Condition type

    // Time parameters (depend on condition type)
    union
    {
        // Time range (CONDITION_TIME_RANGE)
        struct
        {
            uint8_t start_hour;   ///< Start hour (0-23)
            uint8_t start_minute; ///< Start minute (0-59)
            uint8_t end_hour;     ///< End hour (0-23)
            uint8_t end_minute;   ///< End minute (0-59)
            bool spans_midnight;  ///< Spans midnight
        } time_range;

        // Weekday (CONDITION_WEEKDAY)
        struct
        {
            uint8_t weekday_mask; ///< Weekday bit mask (0=Mon, 6=Sun)
        } weekday;

        // Date range (CONDITION_DATE_RANGE)
        struct
        {
            uint8_t start_month;     ///< Start month (1-12)
            uint8_t start_day;       ///< Start day (1-31)
            uint8_t end_month;       ///< End month (1-12)
            uint8_t end_day;         ///< End day (1-31)
            bool recurring_annually; ///< Repeats annually
        } date_range;

        // Special event (CONDITION_SPECIAL_EVENT)
        struct
        {
            uint64_t event_start_timestamp; ///< Event start (epoch s)
            uint64_t event_end_timestamp;   ///< Event end (epoch s)
            uint32_t event_id;              ///< Event ID
        } special_event;
    } params; ///< Condition parameters; the active member is selected by condition_type

    bool is_active;   ///< Condition is active
    uint8_t priority; ///< Priority (0-255, higher = greater)
} rbpower_tariff_condition_t;

/**
 * @brief Tariff zone definition
 */
typedef struct
{
    rbpower_tariff_zone_type_t zone_id;                  ///< Zone ID
    char zone_name[RBPOWER_TARIFF_NAME_LENGTH];          ///< Zone name
    char description[RBPOWER_TARIFF_DESCRIPTION_LENGTH]; ///< Description

    // Tariff rates
    float rate_buying_per_kwh;  ///< Buying rate (currency units/kWh)
    float rate_selling_per_kwh; ///< Selling rate (currency units/kWh)
    float net_metering_ratio;   ///< Net metering ratio (0.0-1.0)

    // Multipliers and adjustments
    float base_multiplier;          ///< Base multiplier (usually 1.0)
    float seasonal_multiplier;      ///< Seasonal multiplier
    float critical_peak_multiplier; ///< Critical peak multiplier

    // Zone application conditions
    rbpower_tariff_condition_t conditions[4]; ///< Up to 4 conditions per zone
    uint8_t conditions_count;                 ///< Number of active conditions
    bool conditions_and_logic;                ///< true=AND, false=OR

    // Zone limits
    float min_duration_minutes; ///< Minimum duration (minutes)
    float max_energy_kwh;       ///< Maximum energy in zone (kWh)
    bool unlimited_energy;      ///< Unlimited energy

    // State and statistics
    bool is_enabled;                    ///< Zone enabled
    bool is_currently_active;           ///< Currently active
    uint64_t last_activation_timestamp; ///< Last activation (epoch s)
    uint32_t activations_count_today;   ///< Activations today

} rbpower_tariff_zone_t;

//=============================================================================
// PROGRESSIVE TARIFF STRUCTURES
//=============================================================================

/**
 * @brief Progressive tariff tier
 */
typedef struct
{
    float threshold_kwh;                        ///< Consumption threshold (kWh)
    float rate_multiplier;                      ///< Rate multiplier
    float fixed_rate_per_kwh;                   ///< Fixed rate (if not multiplier) (currency units/kWh)
    bool use_multiplier;                        ///< true=multiplier, false=fixed rate
    char tier_name[RBPOWER_TARIFF_NAME_LENGTH]; ///< Tier name
} rbpower_tariff_progressive_tier_t;

/**
 * @brief Progressive tariff configuration
 */
typedef struct
{
    bool is_enabled; ///< Progressive tariffs enabled
    rbpower_tariff_progressive_tier_t tiers[RBPOWER_TARIFF_PROGRESSIVE_TIERS]; ///< Tiers ordered by ascending threshold_kwh; tiers_count entries are valid
    uint8_t tiers_count;                           ///< Number of tiers
    rbpower_stat_period_type_t calculation_period; ///< Calculation period (usually MONTHLY)
    bool reset_monthly;                            ///< Reset each month
    uint8_t reset_day;                             ///< Reset day (1-31)
} rbpower_tariff_progressive_config_t;

//=============================================================================
// FIXED PAYMENT STRUCTURES
//=============================================================================

/**
 * @brief Fixed payment
 */
typedef struct
{
    rbpower_tariff_fixed_type_t payment_type;      ///< Payment type
    char payment_name[RBPOWER_TARIFF_NAME_LENGTH]; ///< Payment name

    float amount;                                  ///< Payment amount (currency units)
    char currency[RBPOWER_TARIFF_CURRENCY_LENGTH]; ///< Currency

    // Calculation parameters
    union
    {
        struct
        {
            float per_kw_installed; ///< Per kW installed capacity (currency units/kW)
            float minimum_charge;   ///< Minimum charge (currency units)
        } power_based;

        struct
        {
            float per_kwh_consumed; ///< Per kWh consumed (currency units/kWh)
            float per_kwh_supplied; ///< Per kWh supplied (currency units/kWh)
        } energy_based;

        struct
        {
            float daily_amount;   ///< Daily amount (currency units)
            float monthly_amount; ///< Monthly amount (currency units)
        } time_based;
    } calculation; ///< Calculation parameters; the active member depends on payment_type

    // Application conditions
    bool is_enabled;                           ///< Payment enabled
    rbpower_stat_period_type_t billing_period; ///< Billing period
    uint8_t billing_day;                       ///< Billing day (for monthly)

} rbpower_tariff_fixed_payment_t;

//=============================================================================
// MAIN TARIFF PLAN STRUCTURE
//=============================================================================

/**
 * @brief Complete tariff plan
 */
typedef struct
{
    // === BASIC INFORMATION ===
    char plan_name[RBPOWER_TARIFF_NAME_LENGTH];          ///< Plan name
    char provider_name[RBPOWER_TARIFF_NAME_LENGTH];      ///< Energy provider
    char description[RBPOWER_TARIFF_DESCRIPTION_LENGTH]; ///< Plan description
    rbpower_tariff_plan_type_t plan_type;                ///< Plan type

    // === TARIFF ZONES ===
    rbpower_tariff_zone_t zones[RBPOWER_TARIFF_ZONE_COUNT]; ///< Zone definitions (indexed by rbpower_tariff_zone_type_t)
    uint8_t active_zones_count;                 ///< Number of active zones
    rbpower_tariff_zone_type_t default_zone_id; ///< Default zone

    // === PROGRESSIVE TARIFF ===
    rbpower_tariff_progressive_config_t progressive_config; ///< Progressive (block-tier) pricing configuration

    // === FIXED PAYMENTS ===
    rbpower_tariff_fixed_payment_t fixed_payments[8]; ///< Up to 8 payment types
    uint8_t fixed_payments_count;                     ///< Number of active payments

    // === BASIC SETTINGS ===
    // Currency and region
    char currency_code[RBPOWER_TARIFF_CURRENCY_LENGTH]; ///< ISO 4217 currency code (for example EUR, USD); display label only
    char region_code[RBPOWER_TARIFF_REGION_LENGTH];     ///< Region/country code

    // Taxes and fees (in percent)
    float vat_percent;            ///< VAT (%)
    float energy_tax_per_kwh;     ///< Energy tax (currency units/kWh)
    float grid_usage_fee_per_kwh; ///< Grid usage fee (currency units/kWh)
    float renewable_levy_per_kwh; ///< Renewable levy (currency units/kWh)

    // === NET METERING SETTINGS ===
    bool net_metering_enabled;                      ///< Energy credit enabled
    float net_metering_default_ratio;               ///< Default credit ratio
    rbpower_stat_period_type_t net_metering_period; ///< Credit period (usually MONTHLY)
    bool carry_over_credits;                        ///< Carry over credits to next period
    float max_credit_carry_over_kwh;                ///< Maximum credit carry over (kWh)

    // === SUPPORTED FEATURES ===
    uint32_t supported_features;                            ///< Feature bit mask
    rbpower_tariff_calculation_method_t calculation_method; ///< Calculation method

    // === VALIDITY PERIOD ===
    uint64_t valid_from_timestamp;  ///< Valid from (epoch s)
    uint64_t valid_until_timestamp; ///< Valid until (epoch s)
    uint32_t plan_version;          ///< Plan version

    // === PLAN STATE ===
    bool is_active;                   ///< Plan active
    bool is_default;                  ///< Default plan
    uint64_t created_timestamp;       ///< Creation time (epoch s)
    uint64_t last_modified_timestamp; ///< Last modified (epoch s)

    // === OPTIMIZATION ===
    rbpower_tariff_optimization_priority_t optimization_priority; ///< Optimization priority
    bool enable_load_shifting_recommendations;                    ///< Enable load shifting recommendations
    float load_shifting_threshold_percent;                        ///< Savings threshold for recommendations (%)

} rbpower_tariff_plan_t;

//=============================================================================
// CALENDAR AND SCHEDULE STRUCTURES
//=============================================================================

/**
 * @brief Hourly tariff schedule entry.
 *
 * Reserved data model: not instantiated by the current firmware.
 */
typedef struct
{
    float override_multiplier; ///< Multiplier override (1.0 = no change)
    bool is_critical_peak;     ///< Critical peak
    bool is_maintenance;       ///< Grid maintenance
} rbpower_tariff_hour_schedule_t;

/**
 * @brief Daily tariff schedule.
 *
 * Reserved data model: not instantiated by the current firmware.
 */
typedef struct
{
    rbpower_tariff_hour_schedule_t hours[24]; ///< Schedule by hours

    // Day classification
    bool is_holiday;           ///< Holiday
    bool is_weekend;           ///< Weekend day
    bool is_special_event_day; ///< Special event day

    // Daily adjustments
    float day_multiplier;                        ///< Multiplier for entire day
    rbpower_tariff_zone_type_t override_zone_id; ///< Zone override for entire day
    bool override_entire_day;                    ///< Override entire day

    // Metadata
    char event_description[RBPOWER_TARIFF_DESCRIPTION_LENGTH]; ///< Event description
    uint32_t event_id;                                         ///< Event ID

} rbpower_tariff_day_schedule_t;

/**
 * @brief Simplified tariff calendar.
 *
 * Reserved data model: not instantiated by the current firmware. Holidays are configured with
 *       rbpower_tariff_set_holidays().
 */
typedef struct
{
    // === WEEK TEMPLATES ===
    rbpower_tariff_day_schedule_t week_template[7]; ///< Normal week (0=Mon, 6=Sun)

    // === HOLIDAYS AND SPECIAL DAYS ===
    struct
    {
        uint16_t month_day;                             ///< MMDD format (e.g., 1225 = December 25)
        rbpower_tariff_day_schedule_t special_schedule; ///< Special schedule
        bool recurring_annually;                        ///< Repeats each year
        char holiday_name[RBPOWER_TARIFF_NAME_LENGTH];  ///< Holiday name
    } holidays[RBPOWER_TARIFF_HOLIDAYS_COUNT]; ///< Holidays and special days
    uint8_t holidays_count; ///< Number of holidays

    // === SEASONAL ADJUSTMENTS ===
    struct
    {
        uint8_t start_month;                          ///< Start month (1-12)
        uint8_t start_day;                            ///< Start day (1-31)
        uint8_t end_month;                            ///< End month (1-12)
        uint8_t end_day;                              ///< End day (1-31)
        float rate_adjustment_factor;                 ///< Rate adjustment
        char season_name[RBPOWER_TARIFF_NAME_LENGTH]; ///< Season name
    } seasons[RBPOWER_TARIFF_SEASONS_COUNT]; ///< Seasonal rate adjustments
    uint8_t seasons_count; ///< Number of seasons

    // === CRITICAL PEAKS ===
    struct
    {
        uint64_t start_timestamp;                       ///< Start (epoch s)
        uint64_t end_timestamp;                         ///< End (epoch s)
        float peak_multiplier;                          ///< Rate multiplier
        char reason[RBPOWER_TARIFF_DESCRIPTION_LENGTH]; ///< Peak reason
        bool notification_sent;                         ///< Notification sent
    } critical_peaks[RBPOWER_TARIFF_CRITICAL_PEAKS_COUNT]; ///< Critical-peak events
    uint8_t critical_peaks_count; ///< Number of critical peaks

    // === CALENDAR METADATA ===
    uint32_t calendar_version;    ///< Calendar version
    uint64_t last_sync_timestamp; ///< Last sync (epoch s)
    char timezone[16];            ///< Timezone (e.g., "Europe/Berlin")
    int8_t timezone_offset_hours; ///< UTC offset in hours
    bool dst_enabled;             ///< Automatic daylight saving time

} rbpower_tariff_calendar_t;

//=============================================================================
// ECONOMICS ACCUMULATOR EXTENSIONS FOR TARIFF
//=============================================================================

/**
 * @brief Per-zone, progressive, fixed-charge and net-metering economic accumulators.
 *
 * Reserved data model: not instantiated by the current firmware.
 */
typedef struct
{
    // === TARIFF ZONE ACCUMULATORS ===
    struct
    {
        // Energy in zone
        double consumed_kwh_accumulator; ///< Consumption in zone (kWh)
        double supplied_kwh_accumulator; ///< Supply in zone (kWh)

        // Cost in zone
        double cost_consumed_accumulator;    ///< Consumption cost in zone (currency units)
        double revenue_supplied_accumulator; ///< Supply revenue in zone (currency units)
        double net_cost_accumulator;         ///< Net cost in zone (currency units)

        // Time in zone
        uint32_t total_seconds_in_zone;  ///< Total time in zone (s)
        uint32_t active_seconds_in_zone; ///< Active time (with consumption/supply) (s)

        // Zone statistics
        uint32_t zone_activations_count; ///< Number of zone activations
        uint32_t samples_in_zone;        ///< Samples in this zone

        // Zone extremes
        float max_power_in_zone;     ///< Maximum power in zone
        float min_cost_rate_in_zone; ///< Minimum rate in zone (currency units/kWh)
        float max_cost_rate_in_zone; ///< Maximum rate in zone (currency units/kWh)

    } zones[RBPOWER_TARIFF_ZONE_COUNT]; ///< Accumulators for each zone

    // === PROGRESSIVE TARIFF ===
    struct
    {
        // Tier accumulators
        double kwh_tier_accumulators[RBPOWER_TARIFF_PROGRESSIVE_TIERS];  ///< kWh by tiers (kWh)
        double cost_tier_accumulators[RBPOWER_TARIFF_PROGRESSIVE_TIERS]; ///< Cost by tiers (currency units)
        uint32_t samples_tier_counts[RBPOWER_TARIFF_PROGRESSIVE_TIERS];  ///< Samples by tiers

        // Current tier
        uint8_t current_tier_level; ///< Current tier (0-3)
        double kwh_until_next_tier; ///< kWh until next tier (kWh)
        float current_tier_rate;    ///< Current tier rate (currency units/kWh)

        // Progression statistics
        uint32_t tier_transitions_count;     ///< Number of tier transitions
        uint64_t last_tier_change_timestamp; ///< Last transition time (epoch s)

    } progressive; ///< Progressive (block-tier) accumulators

    // === FIXED CHARGES (allocated) ===
    struct
    {
        // Fixed payment accumulators
        double monthly_fixed_allocated; ///< Allocated monthly subscription (currency units)
        double daily_fixed_allocated;   ///< Allocated daily fee (currency units)
        double power_based_allocated;   ///< Demand charge (allocated) (currency units)
        double grid_usage_allocated;    ///< Grid usage fee (allocated) (currency units)

        // Taxes and fees (allocated)
        double vat_allocated;            ///< VAT (allocated) (currency units)
        double energy_tax_allocated;     ///< Energy tax (currency units)
        double renewable_levy_allocated; ///< Renewable levy (currency units)
        double other_fees_allocated;     ///< Other fees (currency units)

        // Allocation counters
        uint32_t allocation_samples_count; ///< Samples for allocation
        uint32_t billing_days_in_period;   ///< Days in billing period

    } fixed_charges; ///< Allocated fixed charges, taxes and levies

    // === NET METERING AND CREDITS ===
    struct
    {
        // Energy balances
        double net_energy_balance_kwh; ///< Net energy balance (kWh)
        double net_cost_balance;       ///< Net cost balance (currency units)

        // Energy credits
        double energy_credits_earned;  ///< Earned credits (kWh)
        double energy_credits_used;    ///< Used credits (kWh)
        double energy_credits_balance; ///< Credit balance (kWh)

        // Monetary credits
        double monetary_credits_earned;  ///< Earned monetary credits (currency units)
        double monetary_credits_used;    ///< Used monetary credits (currency units)
        double monetary_credits_balance; ///< Monetary credit balance (currency units)

        // Credit statistics
        uint32_t net_metering_transactions;    ///< Credit transactions
        float net_metering_efficiency_percent; ///< Credit efficiency (%)

    } net_metering; ///< Net-metering energy and monetary credit balances

    // === OPTIMIZATION AND POTENTIAL SAVINGS ===
    struct
    {
        // Savings analysis
        double potential_savings_total;   ///< Total savings potential (currency units)
        double achieved_savings;          ///< Achieved savings (currency units)
        double missed_opportunities_cost; ///< Missed opportunities cost (currency units)

        // Load shifting
        uint32_t optimal_shifts_recommended; ///< Recommended shifts
        uint32_t optimal_shifts_executed;    ///< Executed shifts
        double savings_from_shifts;          ///< Savings from shifts (currency units)

        // Peak penalties
        double peak_usage_penalty_cost; ///< Peak usage penalties (currency units)
        uint32_t peak_violations_count; ///< Number of peak violations
        double peak_avoidance_savings;  ///< Peak avoidance savings (currency units)

        // Tariff efficiency
        float tariff_efficiency_score;     ///< Efficiency score (0-100%)
        uint32_t suboptimal_usage_minutes; ///< Minutes of suboptimal usage

    } optimization; ///< Savings potential and load-shifting statistics

    // === TIME ANALYTICS ===
    struct
    {
        // Time of day distribution
        double cost_by_hour[24];            ///< Cost by hour of day (currency units)
        uint32_t usage_minutes_by_hour[24]; ///< Usage minutes by hour (min)

        // Day of week distribution
        double cost_by_weekday[7];            ///< Cost by weekday (currency units)
        uint32_t usage_minutes_by_weekday[7]; ///< Usage minutes by weekday (min)

        // Trend analytics
        double cost_trend_coefficient;  ///< Cost trend coefficient
        double usage_trend_coefficient; ///< Usage trend coefficient

    } time_analytics; ///< Cost and usage distribution by hour and weekday

    // === DATA QUALITY AND VALIDITY ===
    struct
    {
        // Tariff data quality
        uint32_t total_tariff_calculations; ///< Total tariff calculations
        uint32_t successful_calculations;   ///< Successful calculations
        uint32_t failed_calculations;       ///< Failed calculations
        uint32_t tariff_zone_errors;        ///< Zone determination errors

        // Time synchronization
        uint64_t last_tariff_calculation_timestamp; ///< Last tariff calculation (epoch s)
        uint64_t last_zone_determination_timestamp; ///< Last zone determination (epoch s)
        uint32_t time_sync_errors;                  ///< Time sync errors

        // Calculation validity
        bool tariff_data_valid;            ///< Tariff data valid
        uint8_t data_completeness_percent; ///< Data completeness (0-100%)
        uint32_t estimation_samples_count; ///< Samples with estimated data

    } quality_control; ///< Tariff calculation quality counters

} rbpower_accumulator_economics_tariff_t;

//=============================================================================
// MAIN ECONOMICS ACCUMULATOR EXTENSION
//=============================================================================

/**
 * @brief Economics accumulator extended with tariff details (superset of
 *        rbpower_accumulator_economics_t from rbhome_types.h).
 *
 * Reserved data model: not instantiated by the current firmware.
 */
typedef struct
{
    // === EXISTING FIELDS (from rbhome_types.h) ===
    // Basic economic accumulators
    double cost_consumed_accumulator;    ///< Consumption cost accumulator (currency units)
    double revenue_supplied_accumulator; ///< Supply revenue accumulator (currency units)
    double net_cost_accumulator;         ///< Net cost accumulator (currency units)

    // Average tariff accumulators
    double tariff_sum;             ///< Sum of sampled tariff rates (currency units/kWh), for averaging
    uint32_t tariff_samples_count; ///< Tariff sample count

    // Tariff zone time accumulators
    struct
    {
        double cost_peak_accumulator;     ///< Cost during peak hours (currency units)
        double cost_off_peak_accumulator; ///< Cost during off-peak hours (currency units)
        uint64_t peak_time_us;            ///< Time in peak zone (µs)
        uint64_t off_peak_time_us;        ///< Time in off-peak zone (µs)
    } time_of_use; ///< Peak / off-peak cost and time split

    // Efficiency
    double efficiency_sum;             ///< Efficiency sum
    uint32_t efficiency_samples_count; ///< Efficiency sample count

    // === NEW TARIFF FIELDS ===
    rbpower_accumulator_economics_tariff_t tariff; ///< Detailed tariff data

} rbpower_accumulator_economics_extended_t;

//=============================================================================
// INDIVIDUAL DEVICE TARIFF STRUCTURES
//=============================================================================

/**
 * @brief Per-unit direct cost tracking.
 *
 * One instance per home unit (PSRAM). The firmware currently populates
 * device_analytics.cost_distribution_by_zone / revenue_distribution_by_zone and
 * last_cost_calculation_timestamp; session and lifetime fields are reserved.
 */
typedef struct
{
    uint8_t unit_id;                              ///< Device/unit ID
    rbpower_device_profile_type_t device_profile; ///< Device profile

    // === CURRENT COSTS ===
    struct
    {
        double current_session_cost;       ///< Current session cost (currency units)
        uint64_t session_start_timestamp;  ///< Session start (epoch s)
        uint32_t session_duration_seconds; ///< Session duration (s)
        float session_energy_kwh;          ///< Session energy (kWh)
        float session_avg_power_w;         ///< Session average power (W)

    } current_session; ///< Current usage session

    // === ACCUMULATED COSTS ===
    struct
    {
        // Total accumulators
        double lifetime_cost_total;        ///< Total lifetime cost (currency units)
        double lifetime_energy_kwh;        ///< Total lifetime energy (kWh)
        uint32_t lifetime_sessions_count;  ///< Total sessions count
        uint64_t lifetime_runtime_seconds; ///< Total runtime (s)

        // Average metrics
        double cost_per_kwh_avg;     ///< Average cost per kWh (currency units/kWh)
        double cost_per_hour_avg;    ///< Average cost per hour (currency units/h)
        double cost_per_session_avg; ///< Average cost per session (currency units)

    } lifetime_totals; ///< Lifetime totals

    // === DEVICE ANALYTICS ===
    struct
    {
        // Usage patterns
        uint32_t usage_minutes_by_hour[24];                         ///< Usage by hour (min)
        uint32_t usage_count_by_zone[RBPOWER_TARIFF_ZONE_COUNT];    ///< Usage by zone
        float cost_distribution_by_zone[RBPOWER_TARIFF_ZONE_COUNT]; ///< All-in consumption cost attributed to each zone (currency units)
        float revenue_distribution_by_zone[RBPOWER_TARIFF_ZONE_COUNT]; ///< Feed-in (export) revenue attributed to each zone (currency units)

        // Tariff efficiency
        float tariff_efficiency_score;      ///< Efficiency score (0-100%)
        uint32_t suboptimal_sessions_count; ///< Suboptimal sessions
        double total_overpayment;           ///< Total overpayment (currency units)

        // Optimization recommendations
        struct
        {
            bool has_recommendations;          ///< Has recommendations
            uint8_t optimal_hours[5];          ///< Optimal hours of day (0-23, up to 5)
            double monthly_savings_potential;  ///< Monthly savings potential (currency units)
            uint32_t recommended_shifts_count; ///< Recommended shifts
            char suggestion_text[128];         ///< Text suggestion
        } optimization_recommendations;

    } device_analytics; ///< Per-hour and per-zone usage, cost and revenue distribution

    // === METADATA ===
    uint64_t last_cost_calculation_timestamp; ///< Last cost calculation (epoch s)
    uint64_t last_session_end_timestamp;      ///< Last session end (epoch s)
    bool cost_tracking_enabled;               ///< Cost tracking enabled
    uint32_t calculation_errors_count;        ///< Calculation errors count

} rbpower_unit_direct_costs_t;

//=============================================================================
// INTEGRATION WITH EXISTING PERIOD STRUCTURES
//=============================================================================

/**
 * @brief Period statistics extension with tariff economics (complements
 *        rbpower_longterm_economics_t from rbhome_types.h).
 *
 * Reserved data model: not instantiated by the current firmware.
 */
typedef struct
{
    // === EXISTING FIELDS (from rbhome_types.h) ===
    // Detailed economics
    double cost_consumed;     ///< Consumption cost (currency units)
    double revenue_supplied;  ///< Supply revenue (currency units)
    double net_cost;          ///< Net cost (currency units)
    float avg_tariff_per_kwh; ///< Average tariff (currency units/kWh)

    // Tariff zones (if used)
    struct
    {
        double cost_peak_hours;     ///< Cost during peak hours (currency units)
        double cost_off_peak_hours; ///< Cost during off-peak hours (currency units)
        double savings_time_of_use; ///< Savings from time-of-use (currency units)
    } time_of_use; ///< Peak / off-peak cost split

    // Efficiency
    float energy_efficiency_percent; ///< Energy efficiency
    float cost_per_unit_consumed;    ///< Cost per unit consumed (currency units/kWh)

    // === NEW DETAILED TARIFF FIELDS ===
    // Tariff zone breakdown
    struct
    {
        float kwh_consumed;       ///< Consumption in zone (kWh)
        float kwh_supplied;       ///< Supply in zone (kWh)
        double cost_in_zone;      ///< Cost in zone (currency units)
        double revenue_in_zone;   ///< Revenue in zone (currency units)
        uint32_t minutes_in_zone; ///< Minutes in zone
        float avg_rate_in_zone;   ///< Average rate in zone (currency units/kWh)
    } zone_breakdown[RBPOWER_TARIFF_ZONE_COUNT]; ///< Per-zone energy, cost and time breakdown (indexed by rbpower_tariff_zone_type_t)

    // Progressive tariff
    struct
    {
        float kwh_by_tier[RBPOWER_TARIFF_PROGRESSIVE_TIERS];   ///< kWh by tiers (kWh)
        double cost_by_tier[RBPOWER_TARIFF_PROGRESSIVE_TIERS]; ///< Cost by tiers (currency units)
        uint8_t max_tier_reached;                              ///< Maximum tier reached
        double progressive_savings;                            ///< Savings/overpayment from progression (currency units)
    } progressive_breakdown; ///< Progressive (block-tier) breakdown

    // Fixed charges
    struct
    {
        double total_fixed_charges;    ///< Total fixed charges (currency units)
        double monthly_subscription;   ///< Subscription fee (currency units)
        double power_capacity_charges; ///< Capacity charges (currency units)
        double grid_connection_fees;   ///< Connection fees (currency units)
        double taxes_and_levies;       ///< Taxes and levies
    } fixed_charges_breakdown; ///< Fixed charges breakdown

    // Net metering
    struct
    {
        double energy_credits_earned;  ///< Earned credits (kWh)
        double energy_credits_used;    ///< Used credits (kWh)
        double net_metering_savings;   ///< Savings from net metering (currency units)
        float net_metering_efficiency; ///< Net metering efficiency (%)
    } net_metering_summary; ///< Net-metering summary

    // Optimization and potential
    struct
    {
        double potential_savings;             ///< Potential savings (currency units)
        double achieved_optimization_savings; ///< Achieved optimization savings (currency units)
        uint32_t optimal_shifts_executed;     ///< Executed optimal shifts
        float tariff_optimization_score;      ///< Tariff optimization score (0-100%)
    } optimization_summary; ///< Optimization summary

} rbpower_period_economics_tariff_t;

//=============================================================================
// TARIFF SYSTEM CONFIGURATION
//=============================================================================

/**
 * @brief Global tariff engine configuration.
 *
 * Set to defaults by rbpower_tariff_system_init(); most fields are informational.
 */
typedef struct
{
    // === GENERAL SETTINGS ===
    bool tariff_system_enabled;  ///< Tariff system enabled
    bool real_time_calculation;  ///< Real-time calculation
    bool store_detailed_history; ///< Store detailed history

    // === PERFORMANCE ===
    uint8_t calculation_priority;        ///< Tariff task priority (1-10)
    uint16_t max_memory_usage_kb;        ///< Maximum memory for tariff (KiB)
    uint32_t calculation_timeout_ms;     ///< Tariff calculation timeout (ms)
    bool enable_background_optimization; ///< Background optimization

    // === TIME SYNCHRONIZATION ===
    bool use_system_time;                      ///< Use system time (rbtime)
    bool auto_timezone_detection;              ///< Auto timezone detection
    int8_t manual_timezone_offset_hours;       ///< Manual timezone offset
    bool dst_auto_adjustment;                  ///< Automatic DST adjustment
    uint32_t time_sync_check_interval_minutes; ///< Time sync check interval (min)

    // === CALCULATION PERIODS ===
    uint32_t enabled_periods_mask;                     ///< Enabled statistics periods mask
    rbpower_stat_period_type_t primary_billing_period; ///< Primary billing period (usually MONTHLY)
    rbpower_stat_period_type_t optimization_period;    ///< Optimization period (usually DAILY)
    bool enable_real_time_alerts;                      ///< Real-time alerts

    // === FEATURES SUPPORT ===
    uint32_t supported_features;       ///< Supported features (bit mask)
    bool enable_progressive_tariffs;   ///< Progressive tariffs
    bool enable_net_metering;          ///< Net metering
    bool enable_critical_peak_pricing; ///< Critical peak pricing
    bool enable_device_level_tracking; ///< Device-level tracking
    bool enable_load_optimization;     ///< Load optimization

    // === PRECISION AND THRESHOLDS ===
    float min_power_threshold_w;          ///< Minimum power for tariff (W)
    float min_cost_threshold;             ///< Minimum cost for accounting (currency units)
    uint8_t calculation_precision_digits; ///< Calculation precision (decimal digits)
    float rounding_threshold;             ///< Rounding threshold

    // === NOTIFICATIONS AND ALERTS ===
    struct
    {
        bool enable_cost_alerts;              ///< Cost alerts
        float daily_cost_alert_threshold;     ///< Daily cost threshold (currency units)
        float monthly_cost_alert_threshold;   ///< Monthly cost threshold (currency units)
        bool enable_peak_alerts;              ///< Peak alerts
        bool enable_optimization_suggestions; ///< Optimization suggestions
        uint32_t alert_cooldown_minutes;      ///< Alert cooldown interval (min)
    } alerts; ///< Cost / peak alert settings

    // === STORAGE AND EXPORT ===
    struct
    {
        bool enable_nvs_storage;                       ///< NVS storage
        bool enable_file_export;                       ///< File export
        uint32_t history_retention_days;               ///< History retention (days)
        bool compress_historical_data;                 ///< Compress historical data
        rbpower_stat_period_type_t export_granularity; ///< Export granularity
    } storage; ///< Storage and export settings

    // === DEBUG AND DIAGNOSTICS ===
    struct
    {
        bool enable_debug_logging;                 ///< Debug logging
        bool log_all_calculations;                 ///< Log all calculations
        bool enable_performance_monitoring;        ///< Performance monitoring
        uint32_t diagnostic_report_interval_hours; ///< Diagnostic report interval (h)
    } diagnostics; ///< Debug and diagnostic settings

} rbpower_tariff_global_config_t;

/**
 * @brief Per-unit tariff configuration. tariff_tracking_enabled gates costing for the unit.
 */
typedef struct
{
    uint8_t unit_id;              ///< Unit ID
    bool tariff_tracking_enabled; ///< Tariff tracking enabled for this unit

    // === DEVICE PROFILE ===
    rbpower_device_profile_type_t device_profile; ///< Device profile
    char device_name[RBPOWER_TARIFF_NAME_LENGTH]; ///< Device name
    float rated_power_w;                          ///< Rated power (W)
    float rated_energy_kwh;                       ///< Rated energy (kWh)

    // === TARIFF FEATURES ===
    bool enable_individual_optimization;          ///< Individual optimization
    bool enable_session_tracking;                 ///< Session tracking
    bool enable_cost_prediction;                  ///< Cost prediction
    rbpower_tariff_direction_t billing_direction; ///< Billing direction

    // === THRESHOLDS AND LIMITS ===
    struct
    {
        float session_start_power_threshold_w; ///< Session start threshold (W)
        float session_end_power_threshold_w;   ///< Session end threshold (W)
        uint32_t session_timeout_seconds;      ///< Session timeout (s)
        float daily_cost_limit;                ///< Daily cost limit (currency units)
        float monthly_cost_limit;              ///< Monthly cost limit (currency units)
    } thresholds; ///< Session detection thresholds and cost limits

    // === OPTIMIZATION ===
    struct
    {
        rbpower_tariff_optimization_priority_t priority; ///< Optimization priority
        bool allow_load_shifting;                        ///< Allow load shifting
        uint32_t max_shift_delay_minutes;                ///< Maximum shift delay (min)
        float min_savings_threshold_percent;             ///< Minimum savings threshold (%)
        bool prefer_renewable_hours;                     ///< Prefer renewable hours
    } optimization; ///< Load-shifting preferences

    // === NOTIFICATIONS ===
    struct
    {
        bool enable_unit_alerts;                 ///< Unit alerts
        float cost_alert_threshold;              ///< Cost alert threshold (currency units)
        bool notify_on_peak_usage;               ///< Notify on peak usage
        bool notify_on_optimization_opportunity; ///< Notify on optimization opportunity
    } notifications; ///< Unit alert settings

} rbpower_tariff_unit_config_t;

//=============================================================================
// EXTENDED TARIFF CALENDAR
//=============================================================================

/**
 * @brief Extended time condition with multi-interval and seasonal rules.
 *
 * Reserved data model: not instantiated by the current firmware.
 */
typedef struct
{
    rbpower_tariff_condition_type_t condition_type; ///< Condition type

    // === TIME PARAMETERS ===
    union
    {
        // Time range with multiple interval support
        struct
        {
            struct
            {
                uint8_t start_hour;   ///< Start hour (0-23)
                uint8_t start_minute; ///< Start minute (0-59)
                uint8_t end_hour;     ///< End hour (0-23)
                uint8_t end_minute;   ///< End minute (0-59)
                bool is_active;       ///< Interval active
            } intervals[4];           ///< Up to 4 time intervals
            uint8_t intervals_count;  ///< Number of intervals
            bool spans_midnight;      ///< Spans midnight
        } time_range;

        // Extended weekdays with exceptions
        struct
        {
            uint8_t weekday_mask;         ///< Weekday bit mask (1=Mon, 7=Sun)
            uint8_t exclude_weekday_mask; ///< Excluded days
            bool include_holidays;        ///< Include holidays
            bool exclude_holidays;        ///< Exclude holidays
        } weekday;

        // Date range with recurrence
        struct
        {
            uint8_t start_month;     ///< Start month (1-12)
            uint8_t start_day;       ///< Start day (1-31)
            uint8_t end_month;       ///< End month (1-12)
            uint8_t end_day;         ///< End day (1-31)
            bool recurring_annually; ///< Repeats annually
            uint16_t year_start;     ///< Start year (if not recurring)
            uint16_t year_end;       ///< End year (if not recurring)
        } date_range;

        // Seasonal conditions
        struct
        {
            uint8_t season_mask;           ///< Season bit mask (1=winter,2=spring,4=summer,8=fall)
            bool use_astronomical_seasons; ///< Use astronomical seasons
            bool hemisphere_south;         ///< Southern hemisphere
        } seasonal;

        // Special events with notification
        struct
        {
            uint64_t event_start_timestamp;     ///< Event start (epoch s)
            uint64_t event_end_timestamp;       ///< Event end (epoch s)
            uint32_t notification_hours_before; ///< Notification hours before (h)
            uint32_t event_id;                  ///< Event ID
            bool auto_notification_sent;        ///< Auto-notification sent
        } special_event;

        // Temperature/weather conditions (future extension)
        struct
        {
            float temperature_threshold_celsius; ///< Temperature threshold
            bool above_threshold;                ///< true=above, false=below
            uint8_t weather_condition_mask;      ///< Weather conditions mask
        } weather_based;

    } params; ///< Condition parameters; the active member is selected by condition_type

    // === ADDITIONAL PARAMETERS ===
    uint8_t priority; ///< Condition priority (0-255)
    bool is_active;   ///< Condition active
    bool is_override; ///< Overrides other conditions

    // === VALIDITY AND METADATA ===
    uint64_t created_timestamp;                          ///< Creation time (epoch s)
    uint64_t last_evaluation_timestamp;                  ///< Last evaluation (epoch s)
    uint32_t evaluation_count;                           ///< Evaluation count
    uint32_t activation_count;                           ///< Activation count
    char description[RBPOWER_TARIFF_DESCRIPTION_LENGTH]; ///< Condition description

} rbpower_tariff_condition_extended_t;

/**
 * @brief Extended tariff zone with dynamic rates, limits and statistics.
 *
 * Reserved data model: not instantiated by the current firmware.
 */
typedef struct
{
    // === BASIC INFORMATION (from previous structures) ===
    rbpower_tariff_zone_type_t zone_id;                  ///< Zone ID
    char zone_name[RBPOWER_TARIFF_NAME_LENGTH];          ///< Zone name
    char description[RBPOWER_TARIFF_DESCRIPTION_LENGTH]; ///< Description

    // === EXTENDED RATES ===
    struct
    {
        // Base rates
        float base_rate_buying_per_kwh;  ///< Base buying rate (currency units/kWh)
        float base_rate_selling_per_kwh; ///< Base selling rate (currency units/kWh)

        // Dynamic adjustments
        float current_buying_rate;  ///< Current buying rate (currency units/kWh)
        float current_selling_rate; ///< Current selling rate (currency units/kWh)
        float dynamic_multiplier;   ///< Dynamic multiplier

        // Progressive rates within zone
        struct
        {
            float threshold_kw;    ///< Power threshold (kW)
            float rate_multiplier; ///< Multiplier when exceeded
        } power_tiers[3];          ///< Up to 3 power levels
        uint8_t power_tiers_count; ///< Number of levels

    } rates; ///< Base and dynamic rates

    // === EXTENDED CONDITIONS ===
    rbpower_tariff_condition_extended_t conditions[8]; ///< Up to 8 extended conditions
    uint8_t conditions_count;                          ///< Number of conditions
    bool conditions_logic_and;                         ///< true=AND, false=OR

    // === RESTRICTIONS AND LIMITS ===
    struct
    {
        float min_duration_minutes;          ///< Minimum zone duration
        float max_duration_minutes;          ///< Maximum zone duration
        float max_energy_per_activation_kwh; ///< Maximum energy per activation (kWh)
        float max_daily_energy_kwh;          ///< Maximum daily energy (kWh)
        uint8_t max_activations_per_day;     ///< Maximum daily activations
        float cooldown_minutes;              ///< Cooldown period between activations (min)
    } limits; ///< Duration, energy and activation limits

    // === NOTIFICATIONS ===
    struct
    {
        bool notify_on_activation;             ///< Notify on activation
        bool notify_before_activation;         ///< Notify before activation
        uint32_t notification_minutes_before;  ///< How many minutes before to notify (min)
        bool notify_on_rate_change;            ///< Notify on rate change
        float significant_rate_change_percent; ///< Significant rate change (%)
    } notifications; ///< Activation notifications

    // === STATISTICS AND STATUS ===
    struct
    {
        bool is_currently_active;               ///< Currently active
        uint64_t current_activation_start_time; ///< Current activation start (epoch s)
        uint64_t last_activation_timestamp;     ///< Last activation (epoch s)
        uint64_t last_deactivation_timestamp;   ///< Last deactivation (epoch s)

        // Today's statistics
        uint32_t activations_today;          ///< Activations today
        uint32_t total_minutes_active_today; ///< Minutes active today
        float total_energy_today_kwh;        ///< Energy today (kWh)
        double total_cost_today;             ///< Cost today (currency units)

        // Overall statistics
        uint64_t total_activations;         ///< Total activations
        uint64_t total_active_time_minutes; ///< Total active time (min)
        double total_energy_kwh;            ///< Total energy (kWh)
        double total_cost;                  ///< Total cost (currency units)

    } status; ///< Activation statistics

    // === PREDICTIVE DATA ===
    struct
    {
        uint64_t next_predicted_activation;    ///< Next predicted activation (epoch s)
        uint64_t next_predicted_deactivation;  ///< Predicted deactivation (epoch s)
        float predicted_duration_minutes;      ///< Predicted duration
        uint8_t prediction_confidence_percent; ///< Prediction confidence (0-100%)

    } prediction; ///< Next activation prediction

} rbpower_tariff_zone_extended_t;

//=============================================================================
// GLOBAL TARIFF CALENDAR WITH PREDICTIONS
//=============================================================================

/**
 * @brief Full tariff calendar with templates, DST rules and forecasts.
 *
 * Reserved data model: not instantiated by the current firmware.
 */
typedef struct
{
    // === TEMPLATES AND SCHEDULES ===
    // Weekly templates with variants
    struct
    {
        rbpower_tariff_day_schedule_t normal_week[7];  ///< Normal week
        rbpower_tariff_day_schedule_t holiday_week[7]; ///< Holiday week
        rbpower_tariff_day_schedule_t summer_week[7];  ///< Summer week
        rbpower_tariff_day_schedule_t winter_week[7];  ///< Winter week
        uint8_t current_template_id;                   ///< Current template ID (0-3)
    } week_templates; ///< Weekly schedule templates (normal / holiday / summer / winter)

    // === HOLIDAYS AND SPECIAL DAYS (extended) ===
    struct
    {
        uint16_t month_day;                             ///< MMDD format
        rbpower_tariff_day_schedule_t special_schedule; ///< Special schedule
        bool recurring_annually;                        ///< Repeats annually
        char holiday_name[RBPOWER_TARIFF_NAME_LENGTH];  ///< Holiday name
        rbpower_tariff_zone_type_t override_zone;       ///< Overriding zone
        float rate_multiplier;                          ///< Rate multiplier
        bool notify_day_before;                         ///< Notify day before
        uint32_t holiday_category;                      ///< Holiday category
    } holidays[RBPOWER_TARIFF_HOLIDAYS_COUNT]; ///< Holidays and special days
    uint8_t holidays_count; ///< Number of valid entries in holidays[]

    // === EXTENDED SEASONAL ADJUSTMENTS ===
    struct
    {
        uint8_t start_month;                          ///< Start month
        uint8_t start_day;                            ///< Start day
        uint8_t end_month;                            ///< End month
        uint8_t end_day;                              ///< End day
        float rate_adjustment_factor;                 ///< Rate adjustment
        char season_name[RBPOWER_TARIFF_NAME_LENGTH]; ///< Season name

        // Additional parameters
        bool gradual_transition;           ///< Gradual transition
        uint8_t transition_days;           ///< Transition days (days)
        bool weather_dependent;            ///< Weather dependent
        float temperature_trigger_celsius; ///< Temperature trigger

    } seasons[RBPOWER_TARIFF_SEASONS_COUNT]; ///< Seasonal rate adjustments
    uint8_t seasons_count; ///< Number of valid entries in seasons[]

    // === CRITICAL PEAKS (extended) ===
    struct
    {
        uint64_t start_timestamp;                       ///< Start (epoch s)
        uint64_t end_timestamp;                         ///< End (epoch s)
        float peak_multiplier;                          ///< Rate multiplier
        char reason[RBPOWER_TARIFF_DESCRIPTION_LENGTH]; ///< Reason

        // Extended parameters
        rbpower_tariff_zone_type_t affected_zones[4]; ///< Affected zones
        uint8_t affected_zones_count;                 ///< Number of affected zones
        bool automatic_trigger;                       ///< Automatic trigger
        float trigger_threshold_mw;                   ///< System load threshold (MW)

        // Notifications
        bool notification_sent;             ///< Notification sent
        uint32_t notification_hours_before; ///< Notification hours before (h)
        uint8_t notification_priority;      ///< Notification priority (1-10)

    } critical_peaks[RBPOWER_TARIFF_CRITICAL_PEAKS_COUNT]; ///< Critical-peak events
    uint8_t critical_peaks_count; ///< Number of valid entries in critical_peaks[]

    // === TIME SYNCHRONIZATION AND LOCALIZATION ===
    struct
    {
        char timezone[32];              ///< Timezone (IANA format)
        int8_t timezone_offset_hours;   ///< UTC offset
        int8_t timezone_offset_minutes; ///< Additional minutes offset (min)
        bool dst_enabled;               ///< Daylight saving time

        // DST parameters
        struct
        {
            uint8_t start_month;   ///< DST start month
            uint8_t start_week;    ///< Start week (1-5)
            uint8_t start_weekday; ///< Start weekday (0=SUN)
            uint8_t end_month;     ///< End month
            uint8_t end_week;      ///< End week
            uint8_t end_weekday;   ///< End weekday
        } dst_rules;

        // Synchronization
        uint64_t last_sync_timestamp; ///< Last synchronization (epoch s)
        uint32_t sync_interval_hours; ///< Sync interval (h)
        bool auto_sync_enabled;       ///< Auto-sync enabled

    } time_settings; ///< Timezone, DST and synchronization settings

    // === PREDICTIVE ANALYSIS ===
    struct
    {
        // Tariff zone forecast for next 24 hours
        struct
        {
            rbpower_tariff_zone_type_t predicted_zone; ///< Predicted zone
            float predicted_rate;                      ///< Predicted rate (currency units/kWh)
            uint8_t confidence_percent;                ///< Confidence (0-100%)
        } hourly_forecast[24];

        // Optimal periods
        struct
        {
            uint64_t start_timestamp;   ///< Optimal period start (epoch s)
            uint64_t end_timestamp;     ///< Optimal period end (epoch s)
            float rate_benefit_percent; ///< Benefit in percent
            char recommendation[64];    ///< Recommendation
        } optimal_periods[12];          ///< Up to 12 optimal periods per day
        uint8_t optimal_periods_count; ///< Number of valid entries in optimal_periods[]

        // Prediction accuracy statistics
        float prediction_accuracy_percent; ///< Prediction accuracy (%)
        uint32_t correct_predictions;      ///< Correct predictions
        uint32_t total_predictions;        ///< Total predictions

    } predictive; ///< Zone forecast and optimal periods

    // === METADATA AND VERSIONING ===
    uint32_t calendar_version;        ///< Calendar version
    uint64_t created_timestamp;       ///< Creation time (epoch s)
    uint64_t last_modified_timestamp; ///< Last modification (epoch s)
    char creator_info[64];            ///< Creator information
    uint32_t checksum;                ///< Integrity checksum

} rbpower_tariff_calendar_extended_t;

//=============================================================================
// TARIFF CALCULATION RESULTS
//=============================================================================

/**
 * @brief Instant tariff result for one power sample over a short interval.
 *
 * Filled by rbpower_tariff_calculate_instant_cost(). Only the zone, rates, power, cost and
 * period_totals fields are populated; the progressive, net-metering and optimization blocks are
 * left zero.
 */
typedef struct
{
    rbpower_unit_id_t unit_id;      ///< Unit ID
    uint64_t calculation_timestamp; ///< Calculation time (us)
    rbpower_tariff_status_t status; ///< Calculation status

    // === ACTIVE TARIFF ZONE ===
    rbpower_tariff_zone_type_t active_zone_id;         ///< Active zone ID
    char active_zone_name[RBPOWER_TARIFF_NAME_LENGTH]; ///< Active zone name
    float current_buying_rate;                         ///< Current buying rate (currency units/kWh)
    float current_selling_rate;                        ///< Current selling rate (currency units/kWh)

    // === INSTANT VALUES ===
    float instant_power_kw;                      ///< Instant power (kW)
    rbpower_current_direction_t power_direction; ///< Power direction
    float instant_cost_rate_per_hour;            ///< Instant cost (currency units/h)
    float instant_cost_for_period;               ///< Period cost (currency units)

    // === ACCUMULATED VALUES FOR PERIOD ===
    struct
    {
        float energy_consumed_kwh; ///< Energy consumed (kWh)
        float energy_supplied_kwh; ///< Energy supplied (kWh)
        double cost_consumed;      ///< Consumption cost (currency units)
        double revenue_supplied;   ///< Supply revenue (currency units)
        double net_cost;           ///< Net cost (currency units)
    } period_totals; ///< Totals for the costed interval

    // === PROGRESSIVE TARIFF ===
    struct
    {
        uint8_t current_tier_level; ///< Current tier (0-3)
        float tier_threshold_kwh;   ///< Current tier threshold (kWh)
        float kwh_until_next_tier;  ///< kWh until next tier (kWh)
        float tier_rate_multiplier; ///< Current tier multiplier
    } progressive_status; ///< Progressive (block-tier) status

    // === NET METERING ===
    struct
    {
        double energy_credits_balance;   ///< Energy credits balance (kWh)
        double monetary_credits_balance; ///< Monetary credits balance (currency units)
        float net_metering_rate;         ///< Current metering rate (currency units/kWh)
        bool credits_available;          ///< Credits available for use
    } net_metering; ///< Net-metering status

    // === OPTIMIZATION ===
    struct
    {
        bool is_optimal_time;             ///< Optimal time for usage
        float potential_savings_percent;  ///< Potential savings (%)
        uint32_t minutes_until_optimal;   ///< Minutes until optimal time
        char optimization_suggestion[64]; ///< Brief recommendation
    } optimization; ///< Optimization hint

    // === DATA QUALITY ===
    uint8_t data_quality_percent;     ///< Data quality (0-100%)
    bool estimation_used;             ///< Estimated data used
    uint32_t calculation_duration_us; ///< Calculation time (µs)

} rbpower_tariff_instant_result_t;

/**
 * @brief Tariff result for one statistics period of one unit.
 *
 * Filled by rbpower_tariff_get_period_result() from the RAM period accumulator. Fixed charges,
 * progressive breakdown and optimization fields are not implemented and stay zero.
 *
 * @note Energy fields named *_kwh are in kWh (converted from the Wh period accumulators).
 */
typedef struct
{
    rbpower_unit_id_t unit_id;              ///< Unit ID
    rbpower_stat_period_type_t period_type; ///< Period type
    uint64_t period_start_timestamp;        ///< Period start (esp_timer µs)
    uint64_t period_end_timestamp;          ///< Period end (esp_timer µs)
    uint32_t period_duration_minutes;       ///< Period duration (min)

    // === OVERALL TOTALS ===
    struct
    {
        double total_cost;     ///< Total cost (currency units)
        double energy_cost;    ///< Energy cost (currency units)
        double fixed_charges;  ///< Fixed charges (currency units)
        double taxes_and_fees; ///< Taxes and fees (currency units)

        double consumed_cost;    ///< Consumption cost (currency units)
        double supplied_revenue; ///< Supply revenue (currency units)
        double net_cost;         ///< Net cost (currency units)

        // Energy volumes
        float total_energy_consumed_kwh; ///< Total consumption (Wh at present, see note)
        float total_energy_supplied_kwh; ///< Total supply (Wh at present, see note)
        float net_energy_kwh;            ///< Net energy (Wh at present, see note)

        // Average metrics
        float avg_cost_per_kwh; ///< Average cost per kWh (currency units/kWh); not populated (0)
        float avg_power_kw;     ///< Average power (kW)

    } totals; ///< Overall period totals

    // === TARIFF ZONE BREAKDOWN ===
    struct
    {
        rbpower_tariff_zone_type_t zone_id;         ///< Zone ID
        char zone_name[RBPOWER_TARIFF_NAME_LENGTH]; ///< Zone name

        // Energy in zone
        float kwh_consumed; ///< Consumption (Wh at present, see note)
        float kwh_supplied; ///< Supply (kWh)

        // Cost in zone
        double cost_in_zone;     ///< Cost in zone (currency units)
        double revenue_in_zone;  ///< Revenue in zone (currency units)
        double net_cost_in_zone; ///< Net cost in zone (currency units)

        // Time in zone
        uint32_t minutes_in_zone;        ///< Minutes in zone
        uint32_t active_minutes_in_zone; ///< Active minutes in zone (min)
        float zone_utilization_percent;  ///< Zone utilization (%)

        // Zone averages
        float avg_rate_in_zone;  ///< Average rate in zone (currency units/kWh)
        float avg_power_in_zone; ///< Average power in zone

    } zone_breakdown[RBPOWER_TARIFF_ZONE_COUNT]; ///< Per-zone breakdown (indexed by rbpower_tariff_zone_type_t)
    uint8_t zones_used_count; ///< Number of zones used

    // === PROGRESSIVE TARIFF ===
    struct
    {
        bool progressive_applied; ///< Progressive tariffs applied
        uint8_t max_tier_reached; ///< Maximum tier reached

        // Tier breakdown
        struct
        {
            float kwh_in_tier;    ///< kWh in tier (kWh)
            double cost_in_tier;  ///< Cost in tier (currency units)
            float effective_rate; ///< Effective rate (currency units/kWh)
        } tier_breakdown[RBPOWER_TARIFF_PROGRESSIVE_TIERS];

        double progressive_savings; ///< Savings/overcharge from progression (currency units)
        float progressive_rate_avg; ///< Average progressive rate (currency units/kWh)

    } progressive; ///< Progressive (block-tier) breakdown

    // === FIXED CHARGES ===
    struct
    {
        double total_fixed_charges;            ///< Total fixed charges (currency units)
        double monthly_subscription_allocated; ///< Subscription fee (allocated) (currency units)
        double power_capacity_charges;         ///< Power capacity charges (currency units)
        double grid_connection_fees;           ///< Grid connection fees (currency units)
        double vat_amount;                     ///< VAT (currency units)
        double energy_tax_amount;              ///< Energy tax (currency units)
        double renewable_levy_amount;          ///< Renewable energy levy (currency units)
        double other_fees_amount;              ///< Other fees (currency units)

    } fixed_charges; ///< Fixed charges, taxes and levies

    // === NET METERING ===
    struct
    {
        bool net_metering_applied;             ///< Net metering applied
        double energy_credits_earned;          ///< Credits earned (kWh)
        double energy_credits_used;            ///< Credits used (kWh)
        double credits_balance_change;         ///< Credits balance change
        double net_metering_savings;           ///< Metering savings (currency units)
        float net_metering_efficiency_percent; ///< Metering efficiency (%)

    } net_metering; ///< Net-metering summary

    // === ANALYTICS AND OPTIMIZATION ===
    struct
    {
        double potential_savings;           ///< Potential savings (currency units)
        double achieved_savings;            ///< Achieved savings (currency units)
        uint32_t optimal_periods_available; ///< Optimal periods available
        uint32_t optimal_periods_used;      ///< Optimal periods used
        float optimization_score_percent;   ///< Optimization score (0-100%)

        // Recommendations
        struct
        {
            bool has_recommendations;          ///< Has recommendations
            uint32_t load_shift_opportunities; ///< Load shift opportunities
            double potential_monthly_savings;  ///< Potential monthly savings (currency units)
            char primary_recommendation[128];  ///< Primary recommendation
        } recommendations;

    } optimization; ///< Savings and recommendations

    // === METADATA ===
    struct
    {
        uint32_t total_calculations;       ///< Total calculations
        uint32_t successful_calculations;  ///< Successful calculations
        uint8_t data_completeness_percent; ///< Data completeness (%)
        bool period_complete;              ///< Period complete

        // Performance
        uint32_t avg_calculation_time_us; ///< Average calculation time (µs)
        uint32_t max_calculation_time_us; ///< Maximum calculation time (µs)

        // Errors
        uint32_t calculation_errors_count; ///< Calculation errors count
        uint32_t time_sync_errors_count;   ///< Time sync errors count
        char last_error_message[64];       ///< Last error message

    } metadata; ///< Calculation metadata and errors

} rbpower_tariff_period_result_t;

//=============================================================================
// STRUCTURES FOR DEVICES AND SESSIONS
//=============================================================================

/**
 * @brief Device usage session tariff result.
 *
 * Session tracking is not implemented: rbpower_tariff_get_session_result() copies the current
 * session cost and energy from rbpower_unit_direct_costs_t and leaves the rest zero.
 */
typedef struct
{
    rbpower_unit_id_t unit_id;                    ///< Device/unit ID
    uint32_t session_id;                          ///< Session ID
    rbpower_device_profile_type_t device_profile; ///< Device profile

    // === SESSION TIME FRAME ===
    uint64_t session_start_timestamp;  ///< Session start (epoch s)
    uint64_t session_end_timestamp;    ///< Session end (epoch s)
    uint32_t session_duration_minutes; ///< Duration (min)
    bool session_completed;            ///< Session completed

    // === SESSION ENERGY METRICS ===
    float total_energy_consumed_kwh; ///< Total consumption (kWh)
    float avg_power_kw;              ///< Average power (kW)
    float peak_power_kw;             ///< Peak power (kW)
    float min_power_kw;              ///< Minimum power (kW)

    // === SESSION FINANCIAL TOTALS ===
    struct
    {
        double total_session_cost;      ///< Total session cost (currency units)
        double energy_cost;             ///< Energy cost (currency units)
        double fixed_charges_allocated; ///< Allocated fixed charges (currency units)
        double taxes_allocated;         ///< Allocated taxes (currency units)

        // Averages
        float cost_per_kwh;    ///< Cost per kWh (currency units/kWh)
        float cost_per_hour;   ///< Cost per hour (currency units/h)
        float cost_per_minute; ///< Cost per minute (currency units/min)

    } cost_breakdown; ///< Session cost breakdown

    // === TARIFF ZONE USAGE ===
    struct
    {
        rbpower_tariff_zone_type_t zone_id; ///< Zone ID
        uint32_t minutes_in_zone;           ///< Minutes in zone
        float kwh_in_zone;                  ///< kWh in zone (kWh)
        double cost_in_zone;                ///< Cost in zone (currency units)
        float zone_rate;                    ///< Zone rate (currency units/kWh)
        float zone_usage_percent;           ///< Zone usage percent in session
    } zone_usage[RBPOWER_TARIFF_ZONE_COUNT]; ///< Per-zone usage during the session
    uint8_t zones_used_count; ///< Number of zones used

    // === SESSION OPTIMIZATION ===
    struct
    {
        bool was_optimal_timing;   ///< Optimal timing used
        double potential_savings;  ///< Potential savings (currency units)
        double overpayment_amount; ///< Overpayment (currency units)

        // Alternative options
        struct
        {
            uint64_t suggested_start_time; ///< Suggested start time (epoch s)
            uint64_t suggested_end_time;   ///< Suggested end time (epoch s)
            double projected_cost;         ///< Projected cost (currency units)
            double savings_amount;         ///< Savings amount (currency units)
            char reason[64];               ///< Recommendation reason
        } optimization_suggestions[3];     ///< Up to 3 alternatives
        uint8_t suggestions_count; ///< Number of valid entries in optimization_suggestions[]

    } session_optimization; ///< Session timing optimization

    // === COMPARISON WITH PREVIOUS SESSIONS ===
    struct
    {
        double avg_cost_last_10_sessions; ///< Average cost of last 10 sessions (currency units)
        double cost_trend_percent;        ///< Cost trend (%)
        bool cost_above_average;          ///< Cost above average
        float efficiency_score_percent;   ///< Session efficiency score (0-100%)

    } comparative_analysis; ///< Comparison with previous sessions

} rbpower_tariff_session_result_t;

//=============================================================================
// AGGREGATED RESULTS AND REPORTS
//=============================================================================

/**
 * @brief System-wide tariff summary for a period, filled by rbpower_tariff_get_summary_report().
 *
 * Optimization and trend fields are placeholders (zero, or a copy of the current totals).
 *
 * @note Energy fields named *_kwh are in kWh (converted from the Wh period accumulators).
 */
typedef struct
{
    // === GENERAL INFORMATION ===
    rbpower_stat_period_type_t report_period; ///< Report period
    uint64_t report_start_timestamp;          ///< Report period start (epoch s)
    uint64_t report_end_timestamp;            ///< Report period end (epoch s)
    uint64_t report_generated_timestamp;      ///< Report generation time (epoch s)

    // === OVERALL TOTALS FOR ALL UNITS ===
    struct
    {
        uint8_t active_units_count;  ///< Active units count
        double total_system_cost;    ///< Total system cost (currency units)
        double total_energy_cost;    ///< Energy cost (currency units)
        double total_fixed_charges;  ///< Fixed charges (currency units)
        double total_taxes_and_fees; ///< Taxes and fees (currency units)

        float total_energy_consumed_kwh; ///< Total system consumption (Wh at present, see note)
        float total_energy_supplied_kwh; ///< Total system supply (Wh at present, see note)
        float net_energy_balance_kwh;    ///< System energy balance (Wh at present, see note)

        float avg_system_cost_per_kwh; ///< Average system cost per kWh (currency units per Wh at present, see note)

    } system_totals; ///< Totals over all units

    // === UNITS BREAKDOWN ===
    struct
    {
        uint8_t unit_id;                              ///< Unit ID
        char unit_name[RBPOWER_TARIFF_NAME_LENGTH];   ///< Unit name
        rbpower_device_profile_type_t device_profile; ///< Device profile

        // Unit totals
        double unit_total_cost;          ///< Total unit cost (currency units)
        float unit_energy_kwh;           ///< Unit energy (Wh at present, see note)
        float unit_cost_share_percent;   ///< Share of total cost (%)
        float unit_energy_share_percent; ///< Share of total consumption (%)

        // Unit efficiency
        float unit_efficiency_score;   ///< Unit efficiency score
        double unit_potential_savings; ///< Unit potential savings (currency units)

    } units_breakdown[RBPOWER_MAX_HOME_UNITS]; ///< Per-unit breakdown
    uint8_t units_count; ///< Number of valid entries in units_breakdown[]

    // === TARIFF ZONES ===
    struct
    {
        rbpower_tariff_zone_type_t zone_id;         ///< Zone ID
        char zone_name[RBPOWER_TARIFF_NAME_LENGTH]; ///< Zone name

        uint32_t total_minutes_in_zone; ///< Total minutes in zone (min)
        float total_kwh_in_zone;        ///< Total kWh in zone (Wh at present, see note)
        double total_cost_in_zone;      ///< Total cost in zone (currency units)

        float zone_cost_share_percent;   ///< Zone share of total cost (%)
        float zone_energy_share_percent; ///< Zone share of total energy (%)
        float zone_utilization_percent;  ///< Zone utilization (%)

    } zones_summary[RBPOWER_TARIFF_ZONE_COUNT]; ///< Per-zone summary (indexed by rbpower_tariff_zone_type_t)
    uint8_t zones_used_count; ///< Number of zones used in the period

    // === OPTIMIZATION AND RECOMMENDATIONS ===
    struct
    {
        double total_potential_savings;  ///< Total savings potential (currency units)
        double achieved_savings;         ///< Achieved savings (currency units)
        float system_optimization_score; ///< System optimization score (0-100%)

        // Top recommendations
        struct
        {
            char recommendation_text[128];     ///< Recommendation text
            double potential_savings_amount;   ///< Potential savings (currency units)
            uint8_t implementation_difficulty; ///< Implementation difficulty (1-10)
            uint8_t priority;                  ///< Priority (1-10)
        } top_recommendations[5];              ///< Top 5 recommendations
        uint8_t recommendations_count; ///< Number of valid entries in top_recommendations[]

    } optimization_summary; ///< Savings potential and top recommendations

    // === TREND ANALYSIS ===
    struct
    {
        float cost_trend_percent;       ///< Cost trend (%)
        float energy_trend_percent;     ///< Energy consumption trend (%)
        float efficiency_trend_percent; ///< Efficiency trend (%)

        // Forecasts
        double projected_next_period_cost;   ///< Next period cost forecast (currency units)
        float projected_next_period_energy;  ///< Next period consumption forecast (Wh at present, see note)
        uint8_t forecast_confidence_percent; ///< Forecast confidence (%)

    } trend_analysis; ///< Trends and next-period projection

    // === REPORT QUALITY ===
    struct
    {
        uint8_t data_completeness_percent;     ///< Report data completeness (%)
        uint32_t total_calculations_performed; ///< Total calculations performed
        uint32_t calculation_errors_count;     ///< Calculation errors count
        bool report_reliability_high;          ///< High report reliability

        // Generation time
        uint32_t report_generation_time_ms; ///< Report generation time (ms)
        char report_version[16];            ///< Report algorithm version

    } quality_metrics; ///< Report data quality

} rbpower_tariff_summary_report_t;

/**
 * @brief System cost breakdown taken from the Main Supply unit, filled by
 *        rbpower_tariff_get_system_cost_breakdown().
 *
 * @note Energy fields named *_kwh are in kWh (converted from the Wh period accumulators).
 */
typedef struct
{
    rbpower_stat_period_type_t period_type; ///< Period type
    rbpower_unit_id_t main_supply_unit_id;  ///< Main Supply unit ID
    uint64_t timestamp;                     ///< Data timestamp (epoch s)

    // Overall metrics
    double total_system_cost; ///< Total system cost (currency units)
    float total_energy_kwh;   ///< Total system energy (Wh at present, see note)

    // Direction breakdown
    double consumed_cost;      ///< Consumption cost (currency units)
    double supplied_revenue;   ///< Supply revenue (currency units)
    float consumed_energy_kwh; ///< Consumed energy (Wh at present, see note)
    float supplied_energy_kwh; ///< Supplied energy (Wh at present, see note)
    float net_energy_kwh;      ///< Net energy (Wh at present, see note)

    // Averages
    float avg_cost_per_kwh; ///< Average cost per kWh (currency units/kWh); not populated (0)

    // Zone distribution
    double peak_cost;     ///< Peak zone costs (currency units); not populated (0)
    double off_peak_cost; ///< Off-peak zone costs (currency units); not populated (0)

} rbpower_tariff_system_cost_breakdown_t;

//=============================================================================
// API INITIALIZATION AND MANAGEMENT FUNCTIONS
//=============================================================================

/**
 * @brief Initialize the tariff engine.
 *
 * Resets the global configuration, installs the built-in placeholder plan ("Test Day/Night",
 * see rbpower_tariff_plan_is_placeholder()), initializes per-unit tariff configuration and
 * enables the engine. Calling it again after a successful init is a no-op. The timezone is
 * owned by rbtime, not by this module.
 *
 * Call once at startup from the main/init task, before any other tariff function.
 *
 * @return ESP_OK on success (also when already initialized), or the error returned while
 *         building the default plan.
 */
esp_err_t rbpower_tariff_system_init(void);

/**
 * @brief Configure the yearly-recurring holiday list used by HOLIDAY zones and
 *        ::RBPOWER_TARIFF_CONDITION_HOLIDAY.
 *
 * Replaces the previous list. Entries with an out-of-range month or day are skipped; at most
 * ::RBPOWER_TARIFF_HOLIDAYS_COUNT entries are kept. Not thread-safe (writes module state).
 *
 * @param[in] months Month of each entry (1-12); may be NULL when @p count is 0.
 * @param[in] days   Day of month of each entry (1-31); may be NULL when @p count is 0.
 * @param[in] count  Number of entries in @p months and @p days; 0 clears the list.
 * @retval ESP_OK              List replaced.
 * @retval ESP_ERR_INVALID_ARG @p count > 0 and @p months or @p days is NULL.
 */
esp_err_t rbpower_tariff_set_holidays(const uint8_t *months, const uint8_t *days, uint8_t count);

/**
 * @brief Check whether the tariff engine can cost energy.
 *
 * Ready means: initialized, enabled, a plan is active and the wall-clock time is valid
 * (rbtime_is_time_valid()). Logs the individual conditions at INFO level on every call.
 *
 * @return true if all readiness conditions hold, false otherwise.
 */
bool rbpower_tariff_system_is_ready(void);

/**
 * @brief Get the active tariff plan (read-only).
 *
 * The returned pointer refers to ::g_current_tariff_plan, which a plan update can overwrite
 * at any time; copy the fields you need.
 *
 * @return Pointer to the active plan, or NULL if the engine is not initialized.
 */
const rbpower_tariff_plan_t *rbpower_tariff_get_current_plan(void);

/**
 * @brief Get the configured currency label (for example "EUR" or "USD").
 *
 * Display only: no currency conversion is performed anywhere. Use it in user-facing cost
 * messages.
 *
 * @return The plan's currency_code, or "EUR" if none is set. Never NULL.
 */
const char *rbpower_tariff_currency(void);

//=============================================================================
// API TARIFF ZONE DETERMINATION FUNCTIONS
//=============================================================================

/**
 * @brief Get the active tariff zone.
 *
 * Refreshes the zone cache if it is invalid or older than 60 s, then returns the cached
 * zone. Not thread-safe (may write the shared zone cache).
 *
 * @param[out] zone_id      Active zone ID. Must not be NULL.
 * @param[out] zone_name    Buffer of at least ::RBPOWER_TARIFF_NAME_LENGTH bytes for the zone
 *                          name (NUL-terminated); may be NULL.
 * @param[out] current_rate Active buying rate (currency units/kWh); may be NULL.
 * @retval RBPOWER_OK                Zone returned.
 * @retval RBPOWER_ERR_INVALID_ARG   @p zone_id is NULL.
 * @retval RBPOWER_ERR_NOT_INIT      Engine not initialized.
 * @retval RBPOWER_ERR_INVALID_STATE Zone could not be resolved and no valid cached zone exists.
 */
rbpower_err_t rbpower_tariff_get_current_zone(rbpower_tariff_zone_type_t *zone_id,
                                              char *zone_name,
                                              float *current_rate);

/**
 * @brief Refresh the active-zone cache from the current local time.
 *
 * Call periodically (for example from the 3-second period handler) so the hot path
 * rbpower_get_current_tariff_rate_simple() does not resolve time on every call. Logs zone
 * changes. Not thread-safe (writes the shared zone cache).
 *
 * @retval ESP_OK                Cache updated.
 * @retval ESP_ERR_INVALID_STATE Engine not initialized, disabled, or no active plan.
 * @retval ESP_ERR_NOT_FOUND     No zone matches the current time; the cache is invalidated.
 */
esp_err_t rbpower_tariff_update_zone_cache(void);

/**
 * @brief Force a refresh of the active-zone cache.
 *
 * Not thread-safe (writes the shared zone cache).
 *
 * @retval RBPOWER_OK                Cache refreshed (or kept when the zone is unchanged).
 * @retval RBPOWER_ERR_INVALID_STATE Refresh failed (engine not ready or no matching zone).
 */
rbpower_err_t rbpower_tariff_update_active_zone(void);

/**
 * @brief Check whether the active zone changed recently.
 *
 * @note The zone activation time is recorded by two refresh paths with different time bases
 *       (epoch s and esp_timer µs), so treat the result as approximate.
 *
 * @param[in] seconds_threshold Age limit (s).
 * @return true if the cache is valid and the current zone became active no more than
 *         @p seconds_threshold ago, false otherwise.
 */
bool rbpower_tariff_zone_recently_changed(uint32_t seconds_threshold);

//=============================================================================
// API TARIFF CALCULATION FUNCTIONS
//=============================================================================

/**
 * @brief Cost a constant power over a short interval with the active zone's rates.
 *
 * Consumption is priced at the buying rate, export at the selling rate. Fees and VAT are not
 * applied here (see rbpower_update_economics_direct() for the all-in cost). @p result is
 * zeroed first; its calculation_timestamp is esp_timer time (µs).
 *
 * @param[in]  unit_id                 Unit identifier, copied into the result.
 * @param[in]  power_kw                Power (kW, magnitude).
 * @param[in]  period_duration_seconds Interval length (s).
 * @param[in]  direction               Power direction; ::RBPOWER_CURRENT_DIRECTION_SUPPLYING
 *                                     selects the selling rate.
 * @param[out] result                  Calculation result.
 * @retval RBPOWER_TARIFF_STATUS_OK                Result filled.
 * @retval RBPOWER_TARIFF_STATUS_NO_PLAN           Engine not initialized/enabled or no active plan.
 * @retval RBPOWER_TARIFF_STATUS_CALCULATION_ERROR @p result is NULL.
 */
rbpower_tariff_status_t rbpower_tariff_calculate_instant_cost(rbpower_unit_id_t unit_id,
                                                              float power_kw,
                                                              float period_duration_seconds,
                                                              rbpower_current_direction_t direction,
                                                              rbpower_tariff_instant_result_t *result);

//=============================================================================
// API RESULT RETRIEVAL FUNCTIONS
//=============================================================================

/**
 * @brief Cost the unit's latest 200 ms realtime average with the active zone's rates.
 *
 * Wrapper around rbpower_tariff_calculate_instant_cost() with a 0.2 s interval.
 *
 * @param[in]  unit_id Unit index in the home network (0 .. RBPOWER_MAX_HOME_UNITS-1).
 * @param[out] result  Calculation result.
 * @retval RBPOWER_OK                     Result filled.
 * @retval RBPOWER_ERR_INVALID_ARG        @p result is NULL, the index is out of range, or the
 *                                        unit is not defined/active.
 * @retval RBPOWER_ERR_NOT_INIT           Engine not ready (see rbpower_tariff_system_is_ready()).
 * @retval RBPOWER_ERR_INVALID_STATE      Home network unavailable.
 * @retval RBPOWER_ERR_INSUFFICIENT_DATA  No valid realtime average yet.
 * @retval RBPOWER_ERR_CALCULATION_FAILED The cost calculation failed.
 */
rbpower_err_t rbpower_tariff_get_instant_result(rbpower_unit_id_t unit_id,
                                                rbpower_tariff_instant_result_t *result);

/**
 * @brief Get the tariff result of a completed statistics period.
 *
 * Built from the unit's RAM period accumulator, so only the RAM periods
 * (index < RBPOWER_RAM_PERIOD_COUNT) are supported; day and longer periods come from flash
 * statistics instead.
 *
 * @param[in]  unit_id     Unit index in the home network (0 .. RBPOWER_MAX_HOME_UNITS-1).
 * @param[in]  period_type Statistics period (RAM periods only).
 * @param[out] result      Period result (see the unit note on ::rbpower_tariff_period_result_t).
 * @retval RBPOWER_OK                    Result filled.
 * @retval RBPOWER_ERR_INVALID_ARG       NULL @p result, index or period out of range, or the
 *                                       unit is not defined.
 * @retval RBPOWER_ERR_NOT_INIT          Engine not ready.
 * @retval RBPOWER_ERR_INVALID_STATE     Home network unavailable.
 * @retval RBPOWER_ERR_INSUFFICIENT_DATA The period is not complete yet.
 */
rbpower_err_t rbpower_tariff_get_period_result(rbpower_unit_id_t unit_id,
                                               rbpower_stat_period_type_t period_type,
                                               rbpower_tariff_period_result_t *result);

/**
 * @brief Get a device usage session result.
 *
 * Session tracking is not implemented; see ::rbpower_tariff_session_result_t for what is filled.
 *
 * @param[in]  unit_id    Unit index in the home network (0 .. RBPOWER_MAX_HOME_UNITS-1).
 * @param[in]  session_id Session ID, copied into the result.
 * @param[out] result     Session result.
 * @retval RBPOWER_OK              Result filled.
 * @retval RBPOWER_ERR_INVALID_ARG @p result is NULL or the index is out of range.
 * @retval RBPOWER_ERR_NOT_INIT    Engine not ready.
 */
rbpower_err_t rbpower_tariff_get_session_result(rbpower_unit_id_t unit_id,
                                                uint32_t session_id,
                                                rbpower_tariff_session_result_t *result);

/**
 * @brief Build a system-wide tariff summary for a period over all tracked units.
 *
 * @param[in]  period_type Statistics period (RAM periods only, < RBPOWER_RAM_PERIOD_COUNT).
 * @param[out] report      Summary report (see the unit note on ::rbpower_tariff_summary_report_t).
 * @retval RBPOWER_OK                Report filled.
 * @retval RBPOWER_ERR_INVALID_ARG   @p report is NULL or the period is out of range.
 * @retval RBPOWER_ERR_NOT_INIT      Engine not ready.
 * @retval RBPOWER_ERR_INVALID_STATE Home network unavailable.
 */
rbpower_err_t rbpower_tariff_get_summary_report(rbpower_stat_period_type_t period_type,
                                                rbpower_tariff_summary_report_t *report);

/**
 * @brief Hourly cost forecast for a unit.
 *
 * Holds power flat at the unit's current 200 ms average ("if consumption continues") and
 * prices each future hour with the zone that the schedule makes active then. No historical
 * model is used.
 *
 * @param[in]  unit_id          Unit index in the home network (0 .. RBPOWER_MAX_HOME_UNITS-1).
 * @param[in]  forecast_hours   Number of hours to forecast (h); capped at 24.
 * @param[out] forecast_results Array with room for min(@p forecast_hours, 24) results.
 * @param[out] results_count    Number of results written.
 * @retval RBPOWER_OK                     At least one result written.
 * @retval RBPOWER_ERR_INVALID_ARG        NULL pointer, index out of range, or unit not defined.
 * @retval RBPOWER_ERR_NOT_INIT           Engine not ready.
 * @retval RBPOWER_ERR_INVALID_STATE      Home network unavailable.
 * @retval RBPOWER_ERR_INSUFFICIENT_DATA  No valid realtime average yet.
 * @retval RBPOWER_ERR_CALCULATION_FAILED No result could be computed.
 */
rbpower_err_t rbpower_tariff_get_forecast(rbpower_unit_id_t unit_id,
                                          uint32_t forecast_hours,
                                          rbpower_tariff_instant_result_t *forecast_results,
                                          uint8_t *results_count);

//=============================================================================
// API ACCUMULATOR AND INTEGRATION FUNCTIONS
//=============================================================================

//=============================================================================
// API DIAGNOSTICS AND TESTING FUNCTIONS
//=============================================================================

/**
 * @brief Poll the active zone 10 times at 1 s intervals and log zone changes.
 *
 * Blocks the calling task for about 10 s (vTaskDelay). Test helper; do not call from
 * time-critical tasks.
 */
void rbpower_tariff_test_zone_switching(void);

//=============================================================================
// HELPER MACROS
//=============================================================================

/**
 * @brief Evaluate to true if @p status is ::RBPOWER_TARIFF_STATUS_OK.
 */
#define RBPOWER_TARIFF_STATUS_IS_OK(status) \
    ((status) == RBPOWER_TARIFF_STATUS_OK)

/**
 * @brief Evaluate to true if @p status is a critical error (no plan or invalid time).
 */
#define RBPOWER_TARIFF_STATUS_IS_CRITICAL(status) \
    ((status) == RBPOWER_TARIFF_STATUS_NO_PLAN || \
     (status) == RBPOWER_TARIFF_STATUS_INVALID_TIME)

/**
 * @brief Map an ::rbpower_tariff_status_t to an rbpower_err_t.
 *
 * OK → RBPOWER_OK, NO_PLAN → RBPOWER_ERR_NOT_INIT, INVALID_TIME → RBPOWER_ERR_INVALID_STATE,
 * INSUFFICIENT_DATA → RBPOWER_ERR_INSUFFICIENT_DATA, anything else →
 * RBPOWER_ERR_CALCULATION_FAILED.
 */
#define RBPOWER_TARIFF_STATUS_TO_ERR(status)                                                                                               \
    ((status) == RBPOWER_TARIFF_STATUS_OK ? RBPOWER_OK : (status) == RBPOWER_TARIFF_STATUS_NO_PLAN         ? RBPOWER_ERR_NOT_INIT          \
                                                     : (status) == RBPOWER_TARIFF_STATUS_INVALID_TIME      ? RBPOWER_ERR_INVALID_STATE     \
                                                     : (status) == RBPOWER_TARIFF_STATUS_INSUFFICIENT_DATA ? RBPOWER_ERR_INSUFFICIENT_DATA \
                                                                                                           : RBPOWER_ERR_CALCULATION_FAILED)

/**
 * @brief Get the system total cost and energy for a period from the Main Supply unit.
 *
 * Uses the first defined, tariff-tracked unit with role Main Supply and type current-load.
 *
 * @param[in]  period_type         Statistics period (RAM periods only, < RBPOWER_RAM_PERIOD_COUNT).
 * @param[out] total_cost          Net cost of the period (currency units).
 * @param[out] total_energy_kwh    Net energy, import minus export, kWh.
 * @param[out] main_supply_unit_id Index of the Main Supply unit in the home network; may be NULL.
 * @retval RBPOWER_OK                 Values returned.
 * @retval RBPOWER_ERR_INVALID_ARG    @p total_cost or @p total_energy_kwh is NULL, or the period
 *                                    is out of range.
 * @retval RBPOWER_ERR_NOT_INIT       Engine not ready.
 * @retval RBPOWER_ERR_INVALID_STATE  Home network unavailable or the accumulator is disabled.
 * @retval RBPOWER_ERR_NOT_CONFIGURED No tariff-tracked Main Supply unit.
 */
rbpower_err_t rbpower_tariff_get_system_total_cost(rbpower_stat_period_type_t period_type,
                                                   double *total_cost,
                                                   float *total_energy_kwh,
                                                   rbpower_unit_id_t *main_supply_unit_id);

/**
 * @brief Get the detailed system cost breakdown for a period from the Main Supply unit.
 *
 * @param[in]  period_type Statistics period (RAM periods only).
 * @param[out] breakdown   Breakdown (zeroed first; see the unit note on
 *                         ::rbpower_tariff_system_cost_breakdown_t).
 * @retval RBPOWER_OK              Breakdown filled.
 * @retval RBPOWER_ERR_INVALID_ARG @p breakdown is NULL.
 * @return Otherwise the error from rbpower_tariff_get_system_total_cost().
 */
rbpower_err_t rbpower_tariff_get_system_cost_breakdown(rbpower_stat_period_type_t period_type,
                                                       rbpower_tariff_system_cost_breakdown_t *breakdown);

/**
 * @brief Log the system cost breakdown for the 3 s, 15 min, 1 h and daily periods.
 */
void rbpower_tariff_print_system_costs(void);

//=============================================================================
// SIMPLIFIED TARIFF FUNCTIONS FOR 3-SEC PERIOD
//=============================================================================

/**
 * @brief Get the active zone's buying rate (hot path).
 *
 * Returns the cached rate; refreshes the cache once if it is invalid. Not thread-safe (may
 * write the shared zone cache).
 *
 * @return Buying rate (currency units/kWh). Falls back to 0.15 when the engine is not ready or
 *         the zone cannot be resolved.
 */
float rbpower_get_current_tariff_rate_simple(void);

/**
 * @brief Cost one energy increment of a unit and add it to a period result.
 *
 * Import (positive energy) is billed all-in: energy × buying rate (after the progressive tier
 * overlay, if enabled) plus per-kWh grid, tax and levy fees, all multiplied by (1 + VAT).
 * Export (negative energy) is credited at the selling rate without fees or VAT. Updates
 * cost_consumed / revenue_supplied, the energy-weighted average tariff, samples_count and
 * period_end_us of @p result_base, and also:
 * - the unit's lifetime cost/revenue counters,
 * - the per-zone cost/revenue distribution (zone taken from the active-zone cache),
 * - the net-metering billing-period import/export, persisted to NVS on every call.
 *
 * Not thread-safe (writes shared per-unit tables). Call from the statistics task.
 *
 * @param[in]     unit_id      Unit index in the home network (0 .. RBPOWER_MAX_HOME_UNITS-1).
 * @param[in]     energy_kwh   Signed energy of the increment (kWh): + import, − export.
 * @param[in]     buying_rate  Buying rate before fees and VAT (currency units/kWh).
 * @param[in]     selling_rate Selling / feed-in rate (currency units/kWh).
 * @param[in,out] result_base  Period result to update; the call logs an error and returns if NULL.
 */
void rbpower_update_economics_direct(rbpower_unit_id_t unit_id,
                                     float energy_kwh,
                                     float buying_rate,
                                     float selling_rate,
                                     rbgrid_snapshot_base_t *result_base);

/**
 * @brief Cost an injected period result (rbAmp module or ESP-NOW node) with the current tariff.
 *
 * Injected periods do not pass through the 3-second costing path, so the injector calls this.
 * The quarter is priced with the zone active at its midpoint (now − 450 s), so a quarter that
 * ends on a zone boundary is not billed at the next zone's rate; the zone cache is restored
 * afterwards. When a test time is set (rbpower_tariff_set_test_time()) the frozen instant is
 * used without the shift. The cost model is rbpower_update_economics_direct().
 *
 * Not thread-safe (temporarily rewrites the shared zone cache). Call from the statistics task.
 *
 * @param[in]     unit_id     Unit index in the home network (not the unit's definition ID).
 * @param[in]     energy_wh   Signed period energy (Wh): + import, − export.
 * @param[in,out] result_base Period result to update; ignored if NULL.
 */
void rbpower_tariff_cost_injected_period(rbpower_unit_id_t unit_id, float energy_wh,
                                         rbgrid_snapshot_base_t *result_base);

/**
 * @brief Tariff diagnostic snapshot for the `tariffdiag` diagnostic console command.
 *
 * Refreshes the zone cache, then reports the tariff calendar time (the test time if one is
 * set) and the resolved zone and rates. Any output pointer may be NULL.
 *
 * @param[out] hour       Local hour (0-23).
 * @param[out] wday       Weekday (0 = Sunday .. 6 = Saturday).
 * @param[out] mday       Day of month (1-31).
 * @param[out] mon        Month (1-12).
 * @param[out] zone_id    Active zone (::rbpower_tariff_zone_type_t value).
 * @param[out] buying     Buying rate (currency units/kWh).
 * @param[out] selling    Selling rate (currency units/kWh).
 * @param[out] is_holiday 1 if today is a configured holiday, else 0.
 */
void rbpower_tariff_get_diag(int *hour, int *wday, int *mday, int *mon,
                             int *zone_id, float *buying, float *selling, int *is_holiday);

/**
 * @brief Freeze the tariff calendar clock for testing.
 *
 * Lets the `tarifftime` diagnostic command jump to any weekend, holiday, peak or night scenario.
 * Production code never sets it.
 *
 * @param[in] unix_s Frozen time (epoch s), or -1 to use the live clock.
 */
void rbpower_tariff_set_test_time(int64_t unix_s);

/**
 * @brief Get a unit's per-zone consumption cost and feed-in revenue.
 *
 * Both arrays are zero-filled if the unit is not bound.
 *
 * @param[in]  unit_id      Unit definition ID.
 * @param[out] cost_by_zone Array of ::RBPOWER_TARIFF_ZONE_COUNT costs (currency units); may be NULL.
 * @param[out] rev_by_zone  Array of ::RBPOWER_TARIFF_ZONE_COUNT revenues (currency units); may be NULL.
 */
void rbpower_tariff_get_zone_stats(rbpower_unit_id_t unit_id, float *cost_by_zone, float *rev_by_zone);

/**
 * @brief Settle one unit's net-metering balance at the billing-period (monthly) boundary.
 *
 * Nets the period import against export. A net producer banks the surplus as credit
 * (× net-metering ratio, capped at max_credit_carry_over_kwh); a net consumer draws the credit
 * bank down, which reduces the billable net import. Credits expire if carry_over_credits is
 * off. Resets the period accumulators and persists the state to NVS. No-op if the unit is
 * unknown or net metering is disabled in the plan. Not thread-safe.
 *
 * @param[in] unit_id Unit definition ID.
 */
void rbpower_tariff_net_metering_settle(rbpower_unit_id_t unit_id);

/**
 * @brief Read a unit's net-metering state (`netmeter` diagnostic command).
 *
 * Any output pointer may be NULL. All outputs are 0 if the unit is unknown.
 *
 * @param[in]  unit_id             Unit definition ID.
 * @param[out] period_consumed_kwh Import in the current billing period (kWh).
 * @param[out] period_supplied_kwh Export in the current billing period (kWh).
 * @param[out] credit_balance_kwh  Banked credit (kWh).
 * @param[out] lifetime_earned     Total credit ever banked (kWh).
 * @param[out] lifetime_used       Total credit ever used to offset import (kWh).
 * @param[out] last_billable       Net import billed at the last settlement, after credits (kWh).
 */
void rbpower_tariff_get_net_metering(rbpower_unit_id_t unit_id, double *period_consumed_kwh,
                                     double *period_supplied_kwh, double *credit_balance_kwh,
                                     double *lifetime_earned, double *lifetime_used, double *last_billable);

/**
 * @brief Enable or disable progressive (block-tier) pricing at runtime (`progmode` command).
 *
 * Changes the active plan in RAM only. Not thread-safe.
 *
 * @param[in] enabled true to enable the tier overlay on the buying rate.
 */
void rbpower_tariff_set_progressive(bool enabled);

/**
 * @brief Read a unit's progressive-tier state (`progtier` diagnostic command).
 *
 * The active tier is the highest one whose threshold the billing-period import has reached.
 * Any output pointer may be NULL.
 *
 * @param[in]  unit_id        Unit definition ID.
 * @param[out] enabled        Whether progressive pricing is enabled.
 * @param[out] cumulative_kwh Import in the current billing period (kWh).
 * @param[out] tier_index     Active tier index (0 .. RBPOWER_TARIFF_PROGRESSIVE_TIERS-1).
 * @param[out] multiplier     Active tier multiplier, or 1.0 if the tier uses a fixed rate.
 */
void rbpower_tariff_get_progressive(rbpower_unit_id_t unit_id, bool *enabled,
                                    double *cumulative_kwh, int *tier_index, float *multiplier);

/**
 * @brief Cost one unit's completed 3-second period.
 *
 * Prices the period's net energy at the active zone's rates with
 * rbpower_update_economics_direct() and writes the result into the period's result_base.
 * Units that are undefined, inactive or not tariff-tracked, periods still accumulating and
 * energies below 0.1 Wh are skipped and return ESP_OK. Not thread-safe; call from the
 * statistics task.
 *
 * @param[in] unit_id Unit index in the home network (0 .. RBPOWER_MAX_HOME_UNITS-1).
 * @retval ESP_OK                Period costed, skipped, or the engine is not initialized/enabled.
 * @retval ESP_ERR_INVALID_STATE Home network unavailable.
 */
esp_err_t rbpower_tariff_process_3sec_period_for_unit(rbpower_unit_id_t unit_id);

/**
 * @brief Active tariff plan.
 *
 * Holds the built-in placeholder plan after init and the user's plan once one is applied.
 * Not lock-protected: write it only from the configuration path.
 */
extern rbpower_tariff_plan_t g_current_tariff_plan;

/**
 * @brief Check whether a plan is the built-in placeholder ("Test Day/Night" / "Test Provider").
 *
 * The placeholder lets a new hub compute costs before the user configures a tariff; it is not
 * the user's tariff.
 *
 * @param[in] plan_name     Plan name; may be NULL.
 * @param[in] provider_name Provider name; may be NULL.
 * @return true if both names match the placeholder, false otherwise (including NULL input).
 */
#ifdef __cplusplus
extern "C"
#endif
bool rbpower_tariff_plan_is_placeholder(const char* plan_name, const char* provider_name);

#endif // RBHOME_TARIFF_H
