// SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"

namespace esphome {
namespace rbgrid {

/// RBgrid metering core hosted by ESPHome (pilot): starts the core, publishes the system totals every 3 s.
class RBgridComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_power_sensor(sensor::Sensor *s) { power_ = s; }
  void set_energy_in_sensor(sensor::Sensor *s) { energy_in_ = s; }
  void set_energy_out_sensor(sensor::Sensor *s) { energy_out_ = s; }

  /// Set from the core's 3 s callback (esp_timer task): only a flag, the publish runs in loop().
  static std::atomic<bool> frame_ready;

 protected:
  void publish_frame_();

  sensor::Sensor *power_{nullptr};
  sensor::Sensor *energy_in_{nullptr};
  sensor::Sensor *energy_out_{nullptr};
  uint32_t started_ms_{0};
  bool confirmed_{false};
};

}  // namespace rbgrid
}  // namespace esphome
