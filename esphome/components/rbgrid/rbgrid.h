// SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <string>
#include <vector>
#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

namespace esphome {
namespace rbgrid {

/// RBgrid metering core hosted by ESPHome: starts the core, publishes the system totals and the point slots
/// every 3 s, and runs RBgrid commands (`command()`, for an API action).
class RBgridComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_power_sensor(sensor::Sensor *s) { power_ = s; }
  void set_energy_in_sensor(sensor::Sensor *s) { energy_in_ = s; }
  void set_energy_out_sensor(sensor::Sensor *s) { energy_out_ = s; }
  void add_slot(text_sensor::TextSensor *name, sensor::Sensor *p, sensor::Sensor *ein, sensor::Sensor *eout) {
    slots_.push_back(Slot{name, p, ein, eout});
  }

  /// Run one RBgrid command ({"cmd":"get|set|history",...}, the MQTT `cmd` format); returns the JSON reply.
  std::string command(const std::string &json);

  /// Set from the core's 3 s callback (esp_timer task): only a flag, the publish runs in loop().
  static std::atomic<bool> frame_ready;

 protected:
  struct Slot {
    text_sensor::TextSensor *name;
    sensor::Sensor *power;
    sensor::Sensor *energy_in;
    sensor::Sensor *energy_out;
    uint16_t unit{0};
    bool published_empty{false};
  };
  void publish_frame_();
  void load_map_();
  void save_map_();

  sensor::Sensor *power_{nullptr};
  sensor::Sensor *energy_in_{nullptr};
  sensor::Sensor *energy_out_{nullptr};
  std::vector<Slot> slots_;
  bool map_loaded_{false};
  uint32_t started_ms_{0};
  bool confirmed_{false};
};

}  // namespace rbgrid
}  // namespace esphome
