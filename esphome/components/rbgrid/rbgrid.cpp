// SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "rbgrid.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cJSON.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
#include "rbgrid_app.h"
#include "rbgrid_core.h"
#include "rbhome_types.h"   // rbpower_home_lock / unlock
char *rbgrid_json_create_telemetry_frame(uint8_t frame);
}

namespace esphome {
namespace rbgrid {

static const char *const TAG = "rbgrid";
static constexpr uint32_t CONFIRM_AFTER_MS = 60000;   // same grace as the standalone firmware

std::atomic<bool> RBgridComponent::frame_ready{false};

static void on_3s(uint64_t) {
  RBgridComponent::frame_ready.store(true);
  App.wake_loop_threadsafe();
}

// The core start takes seconds (storage, statistics, flash): run it off the ESPHome loop task.
static void start_task(void *) {
  esp_err_t e = rbgrid_host_start();
  ESP_LOGI(TAG, "RBgrid core start: %s", esp_err_to_name(e));
  if (e == ESP_OK) rbgrid_core_register_3s_cb(on_3s);
  vTaskDelete(nullptr);
}

void RBgridComponent::setup() {
  this->started_ms_ = millis();
  if (xTaskCreatePinnedToCore(start_task, "rbgrid_start", 8192, nullptr, 2, nullptr, 0) != pdPASS) {
    ESP_LOGE(TAG, "could not create the start task");
    this->mark_failed();
  }
}

void RBgridComponent::loop() {
  if (!this->confirmed_ && millis() - this->started_ms_ > CONFIRM_AFTER_MS) {
    this->confirmed_ = true;
    rbgrid_core_image_confirmed();   // releases a deferred stats migration; ESPHome confirms the OTA image itself
  }
  if (frame_ready.exchange(false)) this->publish_frame_();
}

void RBgridComponent::publish_frame_() {
  // Copy under the core's lock, publish without it (never hold the home lock across publish_state()).
  rbpower_home_lock();
  char *js = rbgrid_json_create_telemetry_frame(2);
  rbpower_home_unlock();
  if (js == nullptr) return;
  cJSON *root = cJSON_Parse(js);
  free(js);
  if (root == nullptr) return;
  const cJSON *sys = cJSON_GetObjectItem(root, "system");
  auto num = [&](const char *k, sensor::Sensor *s) {
    const cJSON *v = sys ? cJSON_GetObjectItem(sys, k) : nullptr;
    if (s != nullptr && cJSON_IsNumber(v)) s->publish_state((float) v->valuedouble);
  };
  num("p_act", this->power_);
  num("e_in", this->energy_in_);
  num("e_out", this->energy_out_);
  cJSON_Delete(root);
}

void RBgridComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "RBgrid core (hosted), NVS partition: %s", rbgrid_nvs_partition());
  LOG_SENSOR("  ", "Power", this->power_);
  LOG_SENSOR("  ", "Energy in", this->energy_in_);
  LOG_SENSOR("  ", "Energy out", this->energy_out_);
}

}  // namespace rbgrid
}  // namespace esphome
