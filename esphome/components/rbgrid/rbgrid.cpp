// SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "rbgrid.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
#include "rbgrid_app.h"
#include "rbgrid_core.h"
#include "rbhome_types.h"   // rbpower_home_lock / unlock
#include "littlefs_raw.h"   // the slot map lives in the core's config store
#include "cJSON.h"
}

namespace esphome {
namespace rbgrid {

static const char *const TAG = "rbgrid";
static constexpr uint32_t CONFIRM_AFTER_MS = 60000;   // same grace as the standalone firmware
static constexpr int MAX_POINTS = 64;
static const char *const SLOT_MAP_FILE = "/esphome_slots.json";

std::atomic<bool> RBgridComponent::frame_ready{false};
char RBgridComponent::version_buf[9] = {0};
std::atomic<bool> RBgridComponent::version_ready{false};
static std::atomic<bool> s_core_up{false};

// Config version off the loop task: the core builds the points + tariff JSON for it (a few KB, tens of ms).
static void version_task(void *) {
  for (;;) {
    if (s_core_up.load() && !RBgridComponent::version_ready.load()) {
      char v[9];
      rbgrid_core_config_version(v);
      memcpy(RBgridComponent::version_buf, v, sizeof(v));
      RBgridComponent::version_ready.store(true);
    }
    vTaskDelay(pdMS_TO_TICKS(30000));
  }
}

static void on_3s(uint64_t) {
  RBgridComponent::frame_ready.store(true);
  App.wake_loop_threadsafe();
}

// The core start takes seconds (storage, statistics, flash): run it off the ESPHome loop task.
static void start_task(void *) {
  esp_err_t e = rbgrid_host_start();
  ESP_LOGI(TAG, "RBgrid core start: %s", esp_err_to_name(e));
  if (e == ESP_OK) {
    rbgrid_core_register_3s_cb(on_3s);
    s_core_up.store(true);
  }
  vTaskDelete(nullptr);
}

void RBgridComponent::setup() {
  this->started_ms_ = millis();
  if (xTaskCreatePinnedToCore(start_task, "rbgrid_start", 8192, nullptr, 2, nullptr, 0) != pdPASS) {
    ESP_LOGE(TAG, "could not create the start task");
    this->mark_failed();
    return;
  }
  if (this->config_version_ != nullptr)
    xTaskCreatePinnedToCore(version_task, "rbgrid_ver", 6144, nullptr, 1, nullptr, 0);
}

void RBgridComponent::loop() {
  if (!this->confirmed_ && millis() - this->started_ms_ > CONFIRM_AFTER_MS) {
    this->confirmed_ = true;
    rbgrid_core_image_confirmed();   // releases a deferred stats migration; ESPHome confirms the OTA image itself
  }
  if (frame_ready.exchange(false)) this->publish_frame_();
  if (version_ready.load()) {
    std::string v(version_buf);
    version_ready.store(false);
    if (this->config_version_ != nullptr && v != this->version_) {
      this->version_ = v;
      this->config_version_->publish_state(v);
    }
  }
}

// Slot map file: a JSON-ish line of unit ids, one per slot ("[1536,27400,0,...]"); 0 = free.
void RBgridComponent::load_map_() {
  this->map_loaded_ = true;
  char buf[MAX_POINTS * 6 + 8] = {0};
  size_t n = 0;
  if (littlefs_raw_read(SLOT_MAP_FILE, buf, sizeof(buf) - 1, &n) != ESP_OK || n == 0) return;
  buf[n] = '\0';
  const char *p = strchr(buf, '[');
  for (size_t i = 0; p && i < this->slots_.size(); i++) {
    unsigned v = 0;
    if (sscanf(p + 1, "%u", &v) != 1) break;
    this->slots_[i].unit = (uint16_t) v;
    p = strchr(p + 1, ',');
  }
}

void RBgridComponent::save_map_() {
  std::string s = "[";
  for (size_t i = 0; i < this->slots_.size(); i++) {
    if (i) s += ",";
    s += std::to_string(this->slots_[i].unit);
  }
  s += "]";
  esp_err_t e = littlefs_raw_write(SLOT_MAP_FILE, s.c_str(), s.size());
  if (e != ESP_OK) ESP_LOGW(TAG, "slot map not saved: %s", esp_err_to_name(e));
}

void RBgridComponent::publish_frame_() {
  // Copy under the core's lock, publish without it (never hold the home lock across publish_state()).
  static rbgrid_point_snap_t pts[MAX_POINTS];
  rbgrid_system_totals_t st;
  rbpower_home_lock();
  rbgrid_core_system_totals(&st, true);
  int n = rbgrid_core_points_snapshot(pts, MAX_POINTS);
  rbpower_home_unlock();
  rbgrid_tariff_now_t tn;
  rbgrid_core_tariff_now(&tn);

  pub_(this->power_, st.p_w);
  pub_(this->energy_in_, (float) st.e_in_wh);
  pub_(this->energy_out_, (float) st.e_out_wh);
  pub_(this->energy_day_in_, (float) st.e_day_in_wh);
  pub_(this->energy_day_out_, (float) st.e_day_out_wh);
  pub_(this->cost_total_, (float) st.cost_total);
  pub_(this->revenue_total_, (float) st.rev_total);
  pub_(this->cost_day_, (float) st.cost_day);
  pub_(this->revenue_day_, (float) st.rev_day);
  pub_(this->buy_price_, (float) tn.buy_rate_all_in);   // the basis of the cost figures (fees + VAT included)
  pub_(this->sell_price_, tn.sell_rate);
  if (this->tariff_zone_ != nullptr && this->tariff_zone_->state != tn.zone) this->tariff_zone_->publish_state(tn.zone);
  if (this->time_valid_ != nullptr) this->time_valid_->publish_state(tn.time_valid);
  if (this->tariff_placeholder_ != nullptr) this->tariff_placeholder_->publish_state(tn.placeholder);
  if (!this->currency_warned_ && tn.currency[0] && this->currency_ != tn.currency) {
    this->currency_warned_ = true;   // the money unit is fixed at build time; HA only labels it
    ESP_LOGW(TAG, "tariff currency is %s but the money entities are labelled %s (set `currency:` in YAML)", tn.currency,
             this->currency_.c_str());
  }
  if (this->slots_.empty()) return;

  if (!this->map_loaded_) this->load_map_();
  bool changed = false;
  // Free the slots of points that are gone.
  for (auto &sl : this->slots_) {
    if (sl.unit == 0) continue;
    bool found = false;
    for (int k = 0; k < n && !found; k++) found = pts[k].unit_id == sl.unit;
    if (!found) { sl.unit = 0; sl.published_empty = false; changed = true; }
  }
  // Give new points the first free slot, in point-list order.
  for (int k = 0; k < n; k++) {
    bool mapped = false;
    for (auto &sl : this->slots_) if (sl.unit == pts[k].unit_id) { mapped = true; break; }
    if (mapped) continue;
    for (auto &sl : this->slots_) {
      if (sl.unit == 0) { sl.unit = pts[k].unit_id; sl.published_empty = false; changed = true; break; }
    }
  }
  if (changed) this->save_map_();

  for (auto &sl : this->slots_) {
    const rbgrid_point_snap_t *pt = nullptr;
    for (int k = 0; k < n && pt == nullptr; k++) if (pts[k].unit_id == sl.unit) pt = &pts[k];
    if (pt == nullptr) {   // empty slot: publish once
      if (!sl.published_empty) {
        sl.published_empty = true;
        sl.name->publish_state("");
        sl.power->publish_state(NAN);
        sl.energy_in->publish_state(NAN);
        sl.energy_out->publish_state(NAN);
        pub_(sl.current, NAN);
        pub_(sl.voltage, NAN);
      }
      continue;
    }
    const char *label = pt->name[0] ? pt->name : pt->point;
    if (sl.name->state != label) sl.name->publish_state(label);
    sl.power->publish_state(pt->present ? pt->p_w : NAN);   // a point whose source is absent reads unknown
    sl.energy_in->publish_state((float) pt->e_in_wh);
    sl.energy_out->publish_state((float) pt->e_out_wh);
    pub_(sl.current, pt->present ? pt->i_a : NAN);
    pub_(sl.voltage, (pt->present && pt->v_v > 0) ? pt->v_v : NAN);
  }
}

std::string RBgridComponent::command(const std::string &json) {
  char *r = rbgrid_command_dispatch(json.c_str(), (int) json.size());
  if (r == nullptr) return "{\"status\":\"error\",\"message\":\"no reply\"}";
  std::string out(r);
  free(r);
  return out;
}

// The device reply decides success: an error ack (data.result "error"), a flat {"status":"error"} or an HTTP-style
// code >= 400 from a point operation. The message comes back as the action's error.
void RBgridComponent::run_command(const std::string &json) {
  this->last_reply_ = this->command(json);
  this->last_ok_ = true;
  this->last_error_.clear();
  cJSON *r = cJSON_Parse(this->last_reply_.c_str());
  if (r == nullptr) {
    this->last_ok_ = false;
    this->last_error_ = "device reply is not JSON";
    return;
  }
  const cJSON *d = cJSON_GetObjectItem(r, "data");
  const char *st = cJSON_GetStringValue(cJSON_GetObjectItem(r, "status"));
  const char *res = d ? cJSON_GetStringValue(cJSON_GetObjectItem(d, "result")) : nullptr;
  const cJSON *code = d ? cJSON_GetObjectItem(d, "code") : nullptr;
  if ((st && !strcmp(st, "error")) || (res && !strcmp(res, "error")) || (cJSON_IsNumber(code) && code->valueint >= 400)) {
    this->last_ok_ = false;
    const char *m = d ? cJSON_GetStringValue(cJSON_GetObjectItem(d, "message")) : nullptr;
    if (m == nullptr) m = cJSON_GetStringValue(cJSON_GetObjectItem(r, "message"));
    this->last_error_ = m ? m : "command failed";
  }
  cJSON_Delete(r);
}

void RBgridComponent::fill_reply(JsonObject root) {
  root["ok"] = this->last_ok_;
  root["schema"] = 1;
  root["config_version"] = this->version_;
  JsonDocument doc;
  if (deserializeJson(doc, this->last_reply_) == DeserializationError::Ok) {
    if (doc["type"].is<const char *>()) root["type"] = doc["type"].as<const char *>();
    root["data"] = doc.as<JsonVariantConst>();
  } else {
    root["data"] = this->last_reply_;
  }
  this->last_reply_.clear();
  this->last_reply_.shrink_to_fit();
}

void RBgridComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "RBgrid core (hosted), NVS partition: %s, point slots: %u", rbgrid_nvs_partition(),
                (unsigned) this->slots_.size());
  LOG_SENSOR("  ", "Power", this->power_);
  LOG_SENSOR("  ", "Energy in", this->energy_in_);
  LOG_SENSOR("  ", "Energy out", this->energy_out_);
  LOG_SENSOR("  ", "Cost total", this->cost_total_);
  LOG_SENSOR("  ", "Revenue total", this->revenue_total_);
  LOG_TEXT_SENSOR("  ", "Tariff zone", this->tariff_zone_);
  ESP_LOGCONFIG(TAG, "  Money entities labelled: %s", this->currency_.c_str());
}

}  // namespace rbgrid
}  // namespace esphome
