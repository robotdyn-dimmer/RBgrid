# SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""RBgrid metering core as an ESPHome external component (pilot).

The RBgrid core (components/rbpower + components/rbgrid_app of the RBgrid tree) is added as ESP-IDF
components with every RBgrid interface compiled out: ESPHome owns Wi-Fi, the API, OTA and logging.

Points are published through a fixed grid of slots declared once (`point_slots: N`): a new point takes the
first free slot, a deleted one frees it, and the map survives restarts. Adding a module, a point or a node never
needs a YAML change or a rebuild.
"""
import os

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, esp32, sensor, text_sensor
from esphome.const import (
    CONF_ID,
    CONF_NAME,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_MONETARY,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_VOLTAGE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_AMPERE,
    UNIT_VOLT,
    UNIT_WATT,
)

CODEOWNERS = ["@robotdyn-dimmer"]
DEPENDENCIES = ["esp32", "wifi", "psram"]
AUTO_LOAD = ["sensor", "text_sensor", "binary_sensor", "json"]

CONF_RBGRID_PATH = "rbgrid_path"
CONF_NVS_PARTITION = "nvs_partition"
CONF_POWER = "power"
CONF_ENERGY_IN = "energy_in"
CONF_ENERGY_OUT = "energy_out"
CONF_POINT_SLOTS = "point_slots"
CONF_SLOTS = "slots"
CONF_POINT_NAME = "point_name"
CONF_CURRENT = "current"
CONF_VOLTAGE = "voltage"
CONF_SLOT_CURRENT = "slot_current"
CONF_SLOT_VOLTAGE = "slot_voltage"
CONF_CURRENCY = "currency"
CONF_ENERGY_DAY_IN = "energy_day_in"
CONF_ENERGY_DAY_OUT = "energy_day_out"
CONF_COST_TOTAL = "cost_total"
CONF_REVENUE_TOTAL = "revenue_total"
CONF_COST_DAY = "cost_day"
CONF_REVENUE_DAY = "revenue_day"
CONF_BUY_PRICE = "buy_price"
CONF_SELL_PRICE = "sell_price"
CONF_TARIFF_ZONE = "tariff_zone"
CONF_CONFIG_VERSION = "config_version"
CONF_TIME_VALID = "time_valid"
CONF_TARIFF_PLACEHOLDER = "tariff_placeholder"
UNIT_WATT_HOURS = "Wh"
MAX_SLOTS = 64

rbgrid_ns = cg.esphome_ns.namespace("rbgrid")
RBgridComponent = rbgrid_ns.class_("RBgridComponent", cg.Component)

# RBgrid tree root: this file is <root>/esphome/components/rbgrid/__init__.py
_DEFAULT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))

_POWER = dict(unit_of_measurement=UNIT_WATT, accuracy_decimals=1,
              device_class=DEVICE_CLASS_POWER, state_class=STATE_CLASS_MEASUREMENT)
_ENERGY = dict(unit_of_measurement=UNIT_WATT_HOURS, accuracy_decimals=1,
               device_class=DEVICE_CLASS_ENERGY, state_class=STATE_CLASS_TOTAL_INCREASING)

_CURRENT = dict(unit_of_measurement=UNIT_AMPERE, accuracy_decimals=3,
                device_class=DEVICE_CLASS_CURRENT, state_class=STATE_CLASS_MEASUREMENT)
_VOLTAGE = dict(unit_of_measurement=UNIT_VOLT, accuracy_decimals=1,
                device_class=DEVICE_CLASS_VOLTAGE, state_class=STATE_CLASS_MEASUREMENT)

SLOT_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_POINT_NAME): text_sensor.text_sensor_schema(),
        cv.Required(CONF_POWER): sensor.sensor_schema(**_POWER),
        cv.Required(CONF_ENERGY_IN): sensor.sensor_schema(**_ENERGY),
        cv.Required(CONF_ENERGY_OUT): sensor.sensor_schema(**_ENERGY),
        cv.Optional(CONF_CURRENT): sensor.sensor_schema(**_CURRENT),
        cv.Optional(CONF_VOLTAGE): sensor.sensor_schema(**_VOLTAGE),
    }
)


def _money(currency, total):
    """Money: monetary + `total` for lifetime figures (HA refuses total_increasing for monetary); the day figures
    restart at midnight and ESPHome has no last_reset, so they are total_increasing without a device class."""
    if total:
        return dict(unit_of_measurement=currency, accuracy_decimals=4,
                    device_class=DEVICE_CLASS_MONETARY, state_class=STATE_CLASS_TOTAL)
    return dict(unit_of_measurement=currency, accuracy_decimals=4, state_class=STATE_CLASS_TOTAL_INCREASING)


def _system_schema(config):
    """Schemas whose unit depends on `currency:` (validated after it is known)."""
    cur = str(config.get(CONF_CURRENCY, "EUR")).upper()
    config = dict(config)
    config[CONF_CURRENCY] = cur
    schema = cv.Schema(
        {
            cv.Optional(CONF_COST_TOTAL): sensor.sensor_schema(**_money(cur, True)),
            cv.Optional(CONF_REVENUE_TOTAL): sensor.sensor_schema(**_money(cur, True)),
            cv.Optional(CONF_COST_DAY): sensor.sensor_schema(**_money(cur, False)),
            cv.Optional(CONF_REVENUE_DAY): sensor.sensor_schema(**_money(cur, False)),
            cv.Optional(CONF_BUY_PRICE): sensor.sensor_schema(unit_of_measurement=f"{cur}/kWh", accuracy_decimals=4,
                                                              state_class=STATE_CLASS_MEASUREMENT),
            cv.Optional(CONF_SELL_PRICE): sensor.sensor_schema(unit_of_measurement=f"{cur}/kWh", accuracy_decimals=4,
                                                               state_class=STATE_CLASS_MEASUREMENT),
        },
        extra=cv.ALLOW_EXTRA,
    )
    return schema(config)


def _expand_slots(config):
    """`point_slots: N` → N slot entries with generated names, unless `slots:` is given explicitly."""
    config = dict(config)
    n = config.pop(CONF_POINT_SLOTS, 12)
    with_i = config.pop(CONF_SLOT_CURRENT, True)
    with_v = config.pop(CONF_SLOT_VOLTAGE, False)
    if CONF_SLOTS not in config:
        slots = []
        for i in range(1, n + 1):
            sl = {
                CONF_POINT_NAME: {CONF_NAME: f"Point {i} name"},
                CONF_POWER: {CONF_NAME: f"Point {i} power"},
                CONF_ENERGY_IN: {CONF_NAME: f"Point {i} energy in"},
                CONF_ENERGY_OUT: {CONF_NAME: f"Point {i} energy out"},
            }
            if with_i:
                sl[CONF_CURRENT] = {CONF_NAME: f"Point {i} current"}
            if with_v:
                sl[CONF_VOLTAGE] = {CONF_NAME: f"Point {i} voltage"}
            slots.append(sl)
        config[CONF_SLOTS] = slots
    return config


def _require_vfs_dir(config):
    """littlefs (config, logs) needs VFS directory support; ESPHome turns it off unless asked, during validation."""
    esp32.require_vfs_dir()
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema({cv.Optional(CONF_POINT_SLOTS, default=12): cv.int_range(min=0, max=MAX_SLOTS),
               cv.Optional(CONF_SLOT_CURRENT, default=True): cv.boolean,
               cv.Optional(CONF_SLOT_VOLTAGE, default=False): cv.boolean,
               cv.Optional(CONF_CURRENCY, default="EUR"): cv.All(cv.string, cv.Length(min=3, max=3))},
              extra=cv.ALLOW_EXTRA),
    _expand_slots,
    _system_schema,
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(RBgridComponent),
            cv.Optional(CONF_RBGRID_PATH, default=_DEFAULT_ROOT): cv.string,
            cv.Optional(CONF_NVS_PARTITION, default="rbnvs"): cv.string,
            cv.Optional(CONF_POWER): sensor.sensor_schema(**_POWER),
            cv.Optional(CONF_ENERGY_IN): sensor.sensor_schema(**_ENERGY),
            cv.Optional(CONF_ENERGY_OUT): sensor.sensor_schema(**_ENERGY),
            cv.Optional(CONF_ENERGY_DAY_IN): sensor.sensor_schema(**_ENERGY),
            cv.Optional(CONF_ENERGY_DAY_OUT): sensor.sensor_schema(**_ENERGY),
            cv.Optional(CONF_CURRENCY, default="EUR"): cv.string,
            cv.Optional(CONF_COST_TOTAL): cv.valid,
            cv.Optional(CONF_REVENUE_TOTAL): cv.valid,
            cv.Optional(CONF_COST_DAY): cv.valid,
            cv.Optional(CONF_REVENUE_DAY): cv.valid,
            cv.Optional(CONF_BUY_PRICE): cv.valid,
            cv.Optional(CONF_SELL_PRICE): cv.valid,
            cv.Optional(CONF_TARIFF_ZONE): text_sensor.text_sensor_schema(),
            cv.Optional(CONF_CONFIG_VERSION): text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
            cv.Optional(CONF_TIME_VALID): binary_sensor.binary_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
            cv.Optional(CONF_TARIFF_PLACEHOLDER): binary_sensor.binary_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC),
            cv.Optional(CONF_SLOTS, default=[]): cv.All(cv.ensure_list(SLOT_SCHEMA), cv.Length(max=MAX_SLOTS)),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _require_vfs_dir,
)


async def to_code(config):
    root = config[CONF_RBGRID_PATH]
    comps = os.path.join(root, "components")
    for name in ("rbpower", "rbgrid_app", "esp_driver_i2c", "rbbench", "bench_kit"):
        esp32.add_idf_component(name=name, path=os.path.join(comps, name))

    # RBgrid interfaces off: ESPHome owns Wi-Fi, the API, OTA, mDNS and time.
    for key in ("RBGRID_IF_WIFI", "RBGRID_IF_HTTP", "RBGRID_IF_MQTT", "RBGRID_IF_MDNS", "RBGRID_IF_SNTP"):
        esp32.add_idf_sdkconfig_option(f"CONFIG_{key}", False)
    esp32.add_idf_sdkconfig_option("CONFIG_RBGRID_NVS_PARTITION", config[CONF_NVS_PARTITION])
    esp32.add_idf_sdkconfig_option("CONFIG_RBGRID_PANIC_FORENSICS", False)   # ESPHome wraps esp_panic_handler itself
    # rbAmp poll above ESPHome's loopTask (core 1, priority 1): metering cadence never waits for publishing
    esp32.add_idf_sdkconfig_option("CONFIG_RBGRID_POLL_TASK_PRIORITY", 5)
    # PSRAM is required by the core (big tables live there); the `psram:` component turns it on, these are
    # the allocation rules the standalone firmware uses.
    esp32.add_idf_sdkconfig_option("CONFIG_SPIRAM_USE_MALLOC", True)
    esp32.add_idf_sdkconfig_option("CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP", True)
    esp32.add_idf_sdkconfig_option("CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY", True)
    esp32.add_idf_sdkconfig_option("CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL", 16384)
    esp32.add_idf_sdkconfig_option("CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL", 32768)
    esp32.add_idf_sdkconfig_option("CONFIG_ESP_TIMER_TASK_STACK_SIZE", 8192)

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_currency(config[CONF_CURRENCY]))
    for key, setter in ((CONF_POWER, "set_power_sensor"), (CONF_ENERGY_IN, "set_energy_in_sensor"),
                        (CONF_ENERGY_OUT, "set_energy_out_sensor"), (CONF_ENERGY_DAY_IN, "set_energy_day_in_sensor"),
                        (CONF_ENERGY_DAY_OUT, "set_energy_day_out_sensor"), (CONF_COST_TOTAL, "set_cost_total_sensor"),
                        (CONF_REVENUE_TOTAL, "set_revenue_total_sensor"), (CONF_COST_DAY, "set_cost_day_sensor"),
                        (CONF_REVENUE_DAY, "set_revenue_day_sensor"), (CONF_BUY_PRICE, "set_buy_price_sensor"),
                        (CONF_SELL_PRICE, "set_sell_price_sensor")):
        if key in config:
            s = await sensor.new_sensor(config[key])
            cg.add(getattr(var, setter)(s))
    for key, setter in ((CONF_TARIFF_ZONE, "set_tariff_zone_sensor"), (CONF_CONFIG_VERSION, "set_config_version_sensor")):
        if key in config:
            s = await text_sensor.new_text_sensor(config[key])
            cg.add(getattr(var, setter)(s))
    for key, setter in ((CONF_TIME_VALID, "set_time_valid_sensor"),
                        (CONF_TARIFF_PLACEHOLDER, "set_tariff_placeholder_sensor")):
        if key in config:
            s = await binary_sensor.new_binary_sensor(config[key])
            cg.add(getattr(var, setter)(s))
    for slot in config[CONF_SLOTS]:
        name = await text_sensor.new_text_sensor(slot[CONF_POINT_NAME])
        p = await sensor.new_sensor(slot[CONF_POWER])
        ei = await sensor.new_sensor(slot[CONF_ENERGY_IN])
        eo = await sensor.new_sensor(slot[CONF_ENERGY_OUT])
        cur = await sensor.new_sensor(slot[CONF_CURRENT]) if CONF_CURRENT in slot else cg.nullptr
        volt = await sensor.new_sensor(slot[CONF_VOLTAGE]) if CONF_VOLTAGE in slot else cg.nullptr
        cg.add(var.add_slot(name, p, ei, eo, cur, volt))
