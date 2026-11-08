# SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""RBgrid metering core as an ESPHome external component (pilot).

The RBgrid core (components/rbpower + components/rbgrid_app of the RBgrid tree) is added as ESP-IDF
components with every RBgrid interface compiled out: ESPHome owns Wi-Fi, the API, OTA and logging.
"""
import os

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32, sensor
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_POWER,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_WATT,
)

CODEOWNERS = ["@robotdyn-dimmer"]
DEPENDENCIES = ["esp32", "wifi", "psram"]
AUTO_LOAD = ["sensor"]

CONF_RBGRID_PATH = "rbgrid_path"
CONF_NVS_PARTITION = "nvs_partition"
CONF_POWER = "power"
CONF_ENERGY_IN = "energy_in"
CONF_ENERGY_OUT = "energy_out"
UNIT_WATT_HOURS = "Wh"

rbgrid_ns = cg.esphome_ns.namespace("rbgrid")
RBgridComponent = rbgrid_ns.class_("RBgridComponent", cg.Component)

# RBgrid tree root: this file is <root>/esphome/components/rbgrid/__init__.py
_DEFAULT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))

def _require_vfs_dir(config):
    """littlefs (config, logs) needs VFS directory support; ESPHome turns it off unless asked, during validation."""
    esp32.require_vfs_dir()
    return config


CONFIG_SCHEMA = cv.All(cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(RBgridComponent),
        cv.Optional(CONF_RBGRID_PATH, default=_DEFAULT_ROOT): cv.string,
        cv.Optional(CONF_NVS_PARTITION, default="rbnvs"): cv.string,
        cv.Optional(CONF_POWER): sensor.sensor_schema(
            unit_of_measurement=UNIT_WATT, accuracy_decimals=1,
            device_class=DEVICE_CLASS_POWER, state_class=STATE_CLASS_MEASUREMENT),
        cv.Optional(CONF_ENERGY_IN): sensor.sensor_schema(
            unit_of_measurement=UNIT_WATT_HOURS, accuracy_decimals=1,
            device_class=DEVICE_CLASS_ENERGY, state_class=STATE_CLASS_TOTAL_INCREASING),
        cv.Optional(CONF_ENERGY_OUT): sensor.sensor_schema(
            unit_of_measurement=UNIT_WATT_HOURS, accuracy_decimals=1,
            device_class=DEVICE_CLASS_ENERGY, state_class=STATE_CLASS_TOTAL_INCREASING),
    }
).extend(cv.COMPONENT_SCHEMA), _require_vfs_dir)


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
