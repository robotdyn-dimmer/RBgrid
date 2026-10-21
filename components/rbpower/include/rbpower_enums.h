/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbpower_enums.h
 * @brief Core enumerations: error codes, system states, sensor catalogue types, statistics periods
 *        and current direction.
 *
 * The numeric values of the flash-storage enums are part of the on-flash format and must not change.
 */
#ifndef RBPOWER_ENUMS_H
#define RBPOWER_ENUMS_H

/* ================================================================================================
 * ERROR CODES - MINIMUM
 * ================================================================================================ */

/**
 * @brief RBgrid error codes.
 *
 * The HAL/COMPUTE/overload/ZC codes (-1006, -1007, -1018..-1025) come from the former native-ADC
 * measurement path and are kept for ABI stability only.
 */
typedef enum
{
    RBPOWER_OK = 0,                             ///< Successful execution
    RBPOWER_ERR_INVALID_ARG = -1001,            ///< Invalid argument
    RBPOWER_ERR_NOT_INIT = -1002,               ///< Library not initialized
    RBPOWER_ERR_ALREADY_INIT = -1003,           ///< Library already initialized
    RBPOWER_ERR_NOT_STARTED = -1004,            ///< Measurements not started
    RBPOWER_ERR_ALREADY_STARTED = -1005,        ///< Measurements already started
    RBPOWER_ERR_HAL_FAILED = -1006,             ///< HAL layer error (legacy; also returned by some Wi-Fi helpers)
    RBPOWER_ERR_COMPUTE_FAILED = -1007,         ///< COMPUTE layer error (legacy)
    RBPOWER_ERR_NO_MEMORY = -1008,              ///< Memory allocation error
    RBPOWER_ERR_TIMEOUT = -1009,                ///< Operation timeout
    RBPOWER_ERR_INVALID_STATE = -1010,          ///< Invalid state for operation
    RBPOWER_ERR_SENSOR_NOT_CALIBRATED = -1011,  ///< Sensor not calibrated
    RBPOWER_ERR_SENSOR_INVALID_TYPE = -1012,    ///< Unknown sensor type
    RBPOWER_ERR_SENSOR_CAL_FAILED = -1013,      ///< Calibration error
    RBPOWER_ERR_SENSOR_NVS_FAILED = -1014,      ///< NVS operation error
    RBPOWER_ERR_NOT_IMPLEMENTED = -1015,        ///< Function not implemented
    RBPOWER_ERR_FILTER_INVALID = -1016,         ///< Invalid filter parameters
    RBPOWER_ERR_CHANNEL_MISMATCH = -1017,       ///< Sensor type mismatch with channel
    RBPOWER_ERR_LINEAR_OVERLOAD = -1018,        ///< Linear processing overload (legacy native-ADC code)
    RBPOWER_ERR_ZC_NOT_INIT = -1020,            ///< Zero-cross detector not initialized (legacy)
    RBPOWER_ERR_ZC_ALREADY_INIT = -1021,        ///< Zero-cross detector already initialized (legacy)
    RBPOWER_ERR_ZC_GPIO_FAILED = -1022,         ///< Zero-cross GPIO configuration error (legacy)
    RBPOWER_ERR_ZC_ISR_FAILED = -1023,          ///< Zero-cross ISR setup error (legacy)
    RBPOWER_ERR_ZC_NO_EVENTS = -1024,           ///< No zero-cross events for processing (legacy)
    RBPOWER_ERR_ZC_VERIFICATION_FAILED = -1025, ///< Zero-cross event verification failed (legacy)
    // Configuration and initialization errors
    RBPOWER_ERR_NOT_CONFIGURED = -19,     ///< Not configured
    RBPOWER_ERR_CALIBRATION_FAILED = -20, ///< Calibration error
    RBPOWER_ERR_NO_DATA = -21,            ///< No data for processing
    /* Sensor errors */
    RBPOWER_ERR_SENSOR_INIT_FAILED = -1030, ///< Sensor initialization error
    // Wi-Fi errors
    RBPOWER_ERR_WIFI_NO_CONNECTION = -2000, ///< No connection to sensor
    // TIME errors
    RBPOWER_ERR_TIME_NO_FALLBACK = -2501, ///< No fallback time source
    RBPOWER_ERR_TIME_SET_ERROR = -2502,   ///< Time set error
    // Timer errors
    RBPOWER_ERR_TIMER_CREATE_FAILED = -3000, ///< Timer creation error
    RBPOWER_ERR_TIMER_START_FAILED = -3001,  ///< Timer start error
    RBPOWER_ERR_TIMER_STOP_FAILED = -3002,   ///< Timer stop error
    // Tariff errors
    RBPOWER_ERR_INSUFFICIENT_DATA = -4000,      ///< Insufficient data for processing
    RBPOWER_ERR_DATA_PROCESSING_FAILED = -4001, ///< Data processing error
    RBPOWER_ERR_CALCULATION_FAILED = -4002,     ///< Calculation error

    RBPOWER_ERR_NVS_INIT_FAILED = -5000,      ///< NVS initialization error
    RBPOWER_ERR_WIFI_INIT_FAILED = -6001,     ///< Wi-Fi initialization error
    RBPOWER_ERR_WIFI_NOT_INITIALIZED = -6002, ///< Wi-Fi not initialized
    RBPOWER_ERR_WIFI_CONNECT_FAILED = -6003,  ///< Wi-Fi connection error
    RBPOWER_ERR_WIFI_NOT_STARTED = -6004,     ///< Wi-Fi not started
    RBPOWER_ERR_WIFI_ALREADY_STARTED = -6005, ///< Wi-Fi already started

    RBPOWER_ERR_HTTP_INIT_FAILED = -7001,         ///< HTTP server initialization error
    RBPOWER_ERR_HTTP_NOT_INITIALIZED = -7002,     ///< HTTP server not initialized
    RBPOWER_ERR_HTTP_NOT_STARTED = -7003,         ///< HTTP server not started
    RBPOWER_ERR_HTTP_ALREADY_STARTED = -7004,     ///< HTTP server already started
    RBPOWER_ERR_HTTP_URI_REGISTER_FAILED = -7005, ///< HTTP server URI registration error
    RBPOWER_ERR_HTTP_URI_NOT_FOUND = -7006,       ///< URI not found in HTTP server
    RBPOWER_ERR_HTTP_RESPONSE_FAILED = -7007,     ///< HTTP response send error

    // NVS errors
    RBPOWER_ERR_NVS_OPEN_FAILED = -8001,   ///< NVS namespace open error
    RBPOWER_ERR_NVS_WRITE_FAILED = -8002,  ///< NVS write error
    RBPOWER_ERR_NVS_READ_FAILED = -8003,   ///< NVS read error
    RBPOWER_ERR_NVS_NOT_FOUND = -8004,     ///< Data not found in NVS
    RBPOWER_ERR_NVS_COMMIT_FAILED = -8005, ///< NVS commit error

    // AP mode errors
    RBPOWER_ERR_WIFI_AP_INIT_FAILED = -9001,     ///< AP mode initialization error
    RBPOWER_ERR_WIFI_AP_NOT_INITIALIZED = -9002, ///< AP mode not initialized
    RBPOWER_ERR_WIFI_AP_START_FAILED = -9003,    ///< AP mode start error
    RBPOWER_ERR_WIFI_AP_STOP_FAILED = -9004,     ///< AP mode stop error

    // WebSocket errors
    RBPOWER_ERR_WEBSOCKET_INIT_FAILED = -10001, ///< WebSocket initialization error

} rbpower_err_t;

/* ================================================================================================
 * SYSTEM STATES
 * ================================================================================================ */

/** @brief Library/system lifecycle state. */
typedef enum
{
    RBPOWER_STATE_UNINITIALIZED = 0, ///< Not initialized
    RBPOWER_STATE_INITIALIZED,       ///< Initialized
    RBPOWER_STATE_CONFIGURED,        ///< Configured and ready
    RBPOWER_STATE_RUNNING,           ///< Measurements in progress
    RBPOWER_STATE_PAUSED,            ///< Measurements paused
    RBPOWER_STATE_ERROR              ///< Error state
} rbpower_state_t;

/** @brief Log verbosity level. */
typedef enum
{
    RBGRID_LOG_LEVEL_ERROR = 0, ///< Errors
    RBGRID_LOG_LEVEL_WARN,      ///< Warnings
    RBGRID_LOG_LEVEL_INFO,      ///< Information
    RBGRID_LOG_LEVEL_DEBUG      ///< Debug
} rbgrid_loging_level_t;

/**
 * @brief Sensor data source
 */
typedef enum
{
    RBSENSOR_SOURCE_ADC = 0, ///< Analog sensor sampled by an ADC
    RBSENSOR_SOURCE_I2C,     ///< Data from an I2C device
    RBSENSOR_SOURCE_SPI,     ///< Data from an SPI device (reserved)
    RBSENSOR_SOURCE_VIRTUAL  ///< Virtual sensor
} rbsensor_source_t;

/**
 * @brief Sensor measurement class
 */
typedef enum rbsensor_class_s
{
    RBSENSOR_CLASS_VOLTAGE = 0, ///< Voltage sensor
    RBSENSOR_CLASS_CURRENT,     ///< Current sensor
    RBSENSOR_CLASS_POWER,       ///< Power sensor
    RBSENSOR_CLASS_FREQUENCY    ///< Frequency sensor
} rbsensor_class_t;

/**
 * @brief Sensor catalogue types (voltage and current transducers).
 */
typedef enum
{
    // Voltage sensors
    RBSENSOR_TYPE_VOLTAGE_ZMPT101B = 0, ///< ZMPT101B voltage transformer
    RBSENSOR_TYPE_VOLTAGE_ZMPT107_1,    ///< ZMPT107-1 voltage transformer
    RBSENSOR_TYPE_VOLTAGE_DIVIDER,      ///< Resistive voltage divider
    RBSENSOR_TYPE_VOLTAGE_CUSTOM,       ///< Custom voltage sensor

    // Current sensors
    RBSENSOR_TYPE_CURRENT_SCT013_10A,   ///< SCT-013 current transformer, 10 A
    RBSENSOR_TYPE_CURRENT_SCT013_20A,   ///< SCT-013 current transformer, 20 A
    RBSENSOR_TYPE_CURRENT_SCT013_30A,   ///< SCT-013 current transformer, 30 A
    RBSENSOR_TYPE_CURRENT_SCT013_50A,   ///< SCT-013 current transformer, 50 A
    RBSENSOR_TYPE_CURRENT_SCT013_100A,  ///< SCT-013 current transformer, 100 A
    RBSENSOR_TYPE_CURRENT_ACS712_5A,    ///< ACS712 Hall current sensor, 5 A
    RBSENSOR_TYPE_CURRENT_ACS712_20A,   ///< ACS712 Hall current sensor, 20 A
    RBSENSOR_TYPE_CURRENT_ACS712_30A,   ///< ACS712 Hall current sensor, 30 A
    RBSENSOR_TYPE_CURRENT_CUSTOM,       ///< Custom current sensor
    RBSENSOR_TYPE_UNKNOWN               ///< Unknown sensor type
} rbsensor_type_t;


/* ================================================================================================
 * FLASH STORAGE PERIODS (for ring buffers)
 * IMPORTANT: Enum values are critical for compatibility! DO NOT change values!
 * ================================================================================================ */

/**
 * @brief Period types for Flash storage
 */
typedef enum
{
    RBPOWER_PERIOD_HOUR = 0,  ///< Hourly data (720 records = 30 days)
    RBPOWER_PERIOD_DAY = 1,   ///< Daily data (30 records = 30 days)
    RBPOWER_PERIOD_WEEK = 2,  ///< Weekly data (8 records = ~2 months)
    RBPOWER_PERIOD_MONTH = 3, ///< Monthly data (24 records = 2 years)
    RBPOWER_PERIOD_YEAR = 4,  ///< Yearly data (5 records = 5 years)
    RBPOWER_PERIOD_MAX = 5    ///< Number of flash periods
} rbpower_period_t;

/**
 * @brief Data types for Flash storage
 */
typedef enum
{
    RBPOWER_DATA_TYPE_ELECTRICAL = 0, ///< Electrical parameters (28 bytes)
    RBPOWER_DATA_TYPE_ENERGY = 1,     ///< Energy data (16 bytes)
    RBPOWER_DATA_TYPE_ECONOMICS = 2,  ///< Economic indicators (16 bytes)
    RBPOWER_DATA_TYPE_QUALITY = 3,    ///< Power quality (8 bytes)
    RBPOWER_DATA_TYPE_DIRECTION = 4,  ///< Current direction statistics (32 bytes)
    RBPOWER_DATA_TYPE_MAX = 5         ///< Number of data types
} rbpower_data_type_t;

/** @brief Statistical periods (RAM-side period index; DAY and longer map to the flash periods). */
typedef enum
{
    RBPOWER_STAT_PERIOD_3SEC = 0, ///< 3-sec data (RAM only)
    RBPOWER_STAT_PERIOD_15MIN,    ///< 15-min data (RAM only)
    RBPOWER_STAT_PERIOD_1HOUR,    ///< Corresponds to RBPOWER_PERIOD_HOUR
    RBPOWER_STAT_PERIOD_DAILY,    ///< Corresponds to RBPOWER_PERIOD_DAY
    RBPOWER_STAT_PERIOD_WEEKLY,   ///< Corresponds to RBPOWER_PERIOD_WEEK
    RBPOWER_STAT_PERIOD_MONTHLY,  ///< Corresponds to RBPOWER_PERIOD_MONTH
    RBPOWER_STAT_PERIOD_YEARLY,   ///< Corresponds to RBPOWER_PERIOD_YEAR
    RBPOWER_STAT_PERIOD_COUNT = 7 ///< Only 7 statistical periods
} rbpower_stat_period_type_t;

/**
 * @brief Number of periods that are RAM-accumulated (live running sums): 3SEC, 15MIN, 1HOUR.
 *
 * DAILY..YEARLY are recomputed from flash (finalize reads N child records back) and have NO RAM
 * accumulator. g_period_accumulators[][] is sized by this, not RBPOWER_STAT_PERIOD_COUNT, so its
 * second index is only valid for [0..RBPOWER_RAM_PERIOD_COUNT). The RAM results (periods[7]) stay
 * full-size for all periods. Saves ~144 KB of PSRAM at 64 units.
 */
#define RBPOWER_RAM_PERIOD_COUNT 3

/**
 * @brief Current direction
 */
typedef enum
{
    RBPOWER_CURRENT_DIRECTION_UNKNOWN = 0,   ///< Direction unknown
    RBPOWER_CURRENT_DIRECTION_CONSUMING,     ///< Consuming (+P)
    RBPOWER_CURRENT_DIRECTION_SUPPLYING,     ///< Supplying (-P)
    RBPOWER_CURRENT_DIRECTION_BIDIRECTIONAL, ///< Mixed mode
    RBPOWER_CURRENT_DIRECTION_ZERO           ///< Zero current
} rbpower_current_direction_t;

#endif