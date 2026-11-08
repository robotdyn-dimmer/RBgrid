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
from esphome.components import esp32, sensor, text_sensor
from esphome.const import (
    CONF_ID,
    CONF_NAME,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_POWER,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_WATT,
)

CODEOWNERS = ["@robotdyn-dimmer"]
DEPENDENCIES = ["esp32", "wifi", "psram"]
AUTO_LOAD = ["sensor", "text_sensor"]

CONF_RBGRID_PATH = "rbgrid_path"
CONF_NVS_PARTITION = "nvs_partition"
CONF_POWER = "power"
CONF_ENERGY_IN = "energy_in"
CONF_ENERGY_OUT = "energy_out"
CONF_POINT_SLOTS = "point_slots"
CONF_SLOTS = "slots"
CONF_POINT_NAME = "point_name"
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

SLOT_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_POINT_NAME): text_sensor.text_sensor_schema(),
        cv.Required(CONF_POWER): sensor.sensor_schema(**_POWER),
        cv.Required(CONF_ENERGY_IN): sensor.sensor_schema(**_ENERGY),
        cv.Required(CONF_ENERGY_OUT): sensor.sensor_schema(**_ENERGY),
    }
)


def _expand_slots(config):
    """`point_slots: N` → N slot entries with generated names, unless `slots:` is given explicitly."""
    config = dict(config)
    n = config.pop(CONF_POINT_SLOTS, 12)
    if CONF_SLOTS not in config:
        config[CONF_SLOTS] = [
            {
                CONF_POINT_NAME: {CONF_NAME: f"Point {i} name"},
                CONF_POWER: {CONF_NAME: f"Point {i} power"},
                CONF_ENERGY_IN: {CONF_NAME: f"Point {i} energy in"},
                CONF_ENERGY_OUT: {CONF_NAME: f"Point {i} energy out"},
            }
            for i in range(1, n + 1)
        ]
    return config


def _require_vfs_dir(config):
    """littlefs (config, logs) needs VFS directory support; ESPHome turns it off unless asked, during validation."""
    esp32.require_vfs_dir()
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema({cv.Optional(CONF_POINT_SLOTS, default=12): cv.int_range(min=0, max=MAX_SLOTS)}, extra=cv.ALLOW_EXTRA),
    _expand_slots,
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(RBgridComponent),
            cv.Optional(CONF_RBGRID_PATH, default=_DEFAULT_ROOT): cv.string,
            cv.Optional(CONF_NVS_PARTITION, default="rbnvs"): cv.string,
            cv.Optional(CONF_POWER): sensor.sensor_schema(**_POWER),
            cv.Optional(CONF_ENERGY_IN): sensor.sensor_schema(**_ENERGY),
            cv.Optional(CONF_ENERGY_OUT): sensor.sensor_schema(**_ENERGY),
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
    for key in ("RBGRID_IF_WIFI", "RBGRID_IF_HTTP", "RBGRID_IF_MQTT", "RBGRID_IF_MDNS"):
        esp32.add_idf_sdkconfig_option(f"CONFIG_{key}", False)
    esp32.add_idf_sdkconfig_option("CONFIG_RBGRID_NVS_PARTITION", config[CONF_NVS_PARTITION])
    esp32.add_idf_sdkconfig_option("CONFIG_RBGRID_PANIC_FORENSICS", False)   # ESPHome wraps esp_panic_handler itself
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
    for key, setter in ((CONF_POWER, "set_power_sensor"), (CONF_ENERGY_IN, "set_energy_in_sensor"),
                        (CONF_ENERGY_OUT, "set_energy_out_sensor")):
        if key in config:
            s = await sensor.new_sensor(config[key])
            cg.add(getattr(var, setter)(s))
    for slot in config[CONF_SLOTS]:
        name = await text_sensor.new_text_sensor(slot[CONF_POINT_NAME])
        p = await sensor.new_sensor(slot[CONF_POWER])
        ei = await sensor.new_sensor(slot[CONF_ENERGY_IN])
        eo = await sensor.new_sensor(slot[CONF_ENERGY_OUT])
        cg.add(var.add_slot(name, p, ei, eo))
