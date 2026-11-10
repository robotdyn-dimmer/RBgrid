// SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <string>
#include <vector>
#include "esphome/core/component.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"

namespace esphome {
namespace rbgrid {

/// RBgrid metering core hosted by ESPHome: starts the core, publishes the system figures, the tariff and the point
/// slots every 3 s, and runs RBgrid commands for the `rbgrid_cmd` API action.
class RBgridComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  void on_shutdown() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // system
  void set_power_sensor(sensor::Sensor *s) { power_ = s; }
  void set_energy_in_sensor(sensor::Sensor *s) { energy_in_ = s; }
  void set_energy_out_sensor(sensor::Sensor *s) { energy_out_ = s; }
  void set_energy_day_in_sensor(sensor::Sensor *s) { energy_day_in_ = s; }
  void set_energy_day_out_sensor(sensor::Sensor *s) { energy_day_out_ = s; }
  void set_cost_total_sensor(sensor::Sensor *s) { cost_total_ = s; }
  void set_revenue_total_sensor(sensor::Sensor *s) { revenue_total_ = s; }
  void set_cost_day_sensor(sensor::Sensor *s) { cost_day_ = s; }
  void set_revenue_day_sensor(sensor::Sensor *s) { revenue_day_ = s; }
  void set_buy_price_sensor(sensor::Sensor *s) { buy_price_ = s; }
  void set_sell_price_sensor(sensor::Sensor *s) { sell_price_ = s; }
  void set_tariff_zone_sensor(text_sensor::TextSensor *s) { tariff_zone_ = s; }
  void set_price_state_sensor(text_sensor::TextSensor *s) { price_state_ = s; }
  void set_price_source_sensor(text_sensor::TextSensor *s) { price_source_ = s; }
  void set_price_valid_until_sensor(text_sensor::TextSensor *s) { price_valid_until_ = s; }
  void set_provider_price_sensor(sensor::Sensor *s) { provider_price_ = s; }
  void set_config_version_sensor(text_sensor::TextSensor *s) { config_version_ = s; }
  void set_time_valid_sensor(binary_sensor::BinarySensor *s) { time_valid_ = s; }
  void set_tariff_placeholder_sensor(binary_sensor::BinarySensor *s) { tariff_placeholder_ = s; }
  void set_external_price_sensor(binary_sensor::BinarySensor *s) { external_price_ = s; }
  void set_modules_total_sensor(sensor::Sensor *s) { modules_total_ = s; }
  void set_modules_online_sensor(sensor::Sensor *s) { modules_online_ = s; }
  void set_nodes_total_sensor(sensor::Sensor *s) { nodes_total_ = s; }
  void set_nodes_online_sensor(sensor::Sensor *s) { nodes_online_ = s; }
  void set_currency(const std::string &c) { currency_ = c; }
  /// Send the device's event log lines to HA as "esphome.rbgrid_event" {kind, level, message}.
  void set_events(bool on) { events_ = on; }

  void add_slot(text_sensor::TextSensor *name, sensor::Sensor *p, sensor::Sensor *ein, sensor::Sensor *eout,
                sensor::Sensor *cur, sensor::Sensor *volt, sensor::Sensor *cost) {
    slots_.push_back(Slot{name, p, ein, eout, cur, volt, cost});
  }
  /// The source of the last added slot answers (module polled / node online); unknown on an empty slot.
  void set_last_slot_online(binary_sensor::BinarySensor *b) {
    if (!slots_.empty()) slots_.back().online = b;
  }

  /// Run one RBgrid command ({"cmd":"get|set|history|unit",...}); returns the device's JSON reply as text.
  std::string command(const std::string &json);
  /// Run one command for the API action: keeps the reply for last_ok() / last_error() / fill_reply().
  void run_command(const std::string &json);
  /// Whether the last run_command() succeeded (the device reply carried no error).
  bool last_ok() const { return last_ok_; }
  /// Error text of the last run_command() ("" on success).
  std::string last_error() const { return last_error_; }
  /// Action response: {"ok", "schema", "type", "config_version", "data": <the device reply as an object>}.
  void fill_reply(JsonObject root);

  /// Set from the core's 3 s callback (esp_timer task): only a flag, the publish runs in loop().
  static std::atomic<bool> frame_ready;
  /// Config version computed off the loop task (rbgrid_core_config_version builds a few KB of JSON).
  static char version_buf[9];
  static std::atomic<bool> version_ready;
  /// Ask the version task to recompute now (after a command that may change the configuration).
  static void version_kick();

 protected:
  struct Slot {
    text_sensor::TextSensor *name;
    sensor::Sensor *power;
    sensor::Sensor *energy_in;
    sensor::Sensor *energy_out;
    sensor::Sensor *current;   // optional
    sensor::Sensor *voltage;   // optional
    sensor::Sensor *cost;      // optional
    binary_sensor::BinarySensor *online{nullptr};   // optional
    uint16_t unit{0};
    bool published_empty{false};
  };
  void publish_frame_();
  void fire_event_(const char *msg, bool fail, uint32_t seq);
  bool events_{false};
  void load_map_();
  void save_map_();
  static void pub_(sensor::Sensor *s, float v) {
    if (s != nullptr) s->publish_state(v);
  }

  sensor::Sensor *power_{nullptr};
  sensor::Sensor *energy_in_{nullptr};
  sensor::Sensor *energy_out_{nullptr};
  sensor::Sensor *energy_day_in_{nullptr};
  sensor::Sensor *energy_day_out_{nullptr};
  sensor::Sensor *cost_total_{nullptr};
  sensor::Sensor *revenue_total_{nullptr};
  sensor::Sensor *cost_day_{nullptr};
  sensor::Sensor *revenue_day_{nullptr};
  sensor::Sensor *buy_price_{nullptr};
  sensor::Sensor *sell_price_{nullptr};
  sensor::Sensor *modules_total_{nullptr};
  sensor::Sensor *modules_online_{nullptr};
  sensor::Sensor *nodes_total_{nullptr};
  sensor::Sensor *nodes_online_{nullptr};
  text_sensor::TextSensor *tariff_zone_{nullptr};
  text_sensor::TextSensor *price_state_{nullptr};
  text_sensor::TextSensor *price_source_{nullptr};
  text_sensor::TextSensor *price_valid_until_{nullptr};
  sensor::Sensor *provider_price_{nullptr};
  text_sensor::TextSensor *config_version_{nullptr};
  binary_sensor::BinarySensor *time_valid_{nullptr};
  binary_sensor::BinarySensor *tariff_placeholder_{nullptr};
  binary_sensor::BinarySensor *external_price_{nullptr};
  std::string currency_{"EUR"};
  bool currency_warned_{false};
  std::string version_{""};

  std::vector<Slot> slots_;
  bool map_loaded_{false};
  uint32_t started_ms_{0};
  bool confirmed_{false};
  uint32_t confirm_check_ms_{0};

  std::string last_reply_;
  bool last_ok_{false};
  std::string last_error_;
  // A reply over the ESPHome JSON cap (5120 B) goes out in base64 parts: the whole reply stays here until the next
  // command (or 120 s), the client fetches the other parts with {"cmd":"part","rid":R,"part":i}.
  std::string cache_;
  uint32_t cache_rid_{0};
  uint32_t cache_ms_{0};
  int part_req_{-1};
};

}  // namespace rbgrid
}  // namespace esphome
