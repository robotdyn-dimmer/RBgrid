// SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "rbgrid.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cmath>
#include "esp_random.h"
#include <cstdio>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "esp_ota_ops.h"

extern "C" {
#include "rbgrid_app.h"
#include "rbgrid_core.h"
#include "rbhome_types.h"   // rbpower_home_lock / unlock
#include "littlefs_raw.h"   // the slot map lives in the core's config store
#include "cJSON.h"
#include "esp_now_hub.h"
#include "rbamp_bridge.h"   // module counts   // points switched off by the user free their slots
#include "rbevlog.h"    // event lines → HA events
#include "rbdynprice.h"  // dynamic prices: HA is a provider here
}
#include "freertos/queue.h"
#ifdef USE_API
#include "esphome/components/api/api_server.h"
#endif

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
static TaskHandle_t s_version_task = nullptr;

// Event log lines → HA events: copied into a queue in the caller's task, fired from loop().
struct EvItem {
  char msg[160];
  bool fail;
  uint32_t seq;   // event-log sequence (same as `get events` lines): HA's replay cursor
};
static QueueHandle_t s_ev_q = nullptr;
static void on_event_line(const char *msg, bool fail, uint32_t seq) {
  if (s_ev_q == nullptr) return;
  EvItem it;
  strncpy(it.msg, msg, sizeof(it.msg) - 1);
  it.msg[sizeof(it.msg) - 1] = '\0';
  it.fail = fail;
  it.seq = seq;
  if (xQueueSend(s_ev_q, &it, 0) == pdTRUE) App.wake_loop_threadsafe();
}

/// Stable event kinds for automations (blueprints), from the event text. Unknown lines: "fail" / "event".
static const char *event_kind(const char *m, bool fail) {
  struct K {
    const char *needle;
    const char *kind;
  };
  static const K KINDS[] = {
      {"reverse flow", "reverse_flow"},          {"not found at boot", "module_absent"},
      {"ABSENT", "module_absent"},               {"no rbAmp modules", "module_absent"},
      {"joined late", "module_restored"},        {"poll restored", "module_restored"},
      {" poll:", "module_poll_fail"},            {"held low", "bus_stuck"},
      {"(node disabled)", "node_disabled"},      {"espnow: node", "node"},                  {"boot: reset", "boot"},
      {"tariff", "tariff"},                      {"OTA", "firmware"},
  };
  // dynamic prices: "price: …" / "FAIL price: …" at the line start
  if (!strncmp(m, "price:", 6) || !strncmp(m, "FAIL price:", 11)) return "price";
  // reboot-gap / period reconciliation lines ("m0 gap …", "QTR m0 … anchor=…", "cnt=+nanWh"): diagnostics, not news
  if (strstr(m, "anchor=") || strstr(m, " cnt=+") || strstr(m, " cnt=-") || strstr(m, "QTR m")) return "diag";
  for (const char *g = strstr(m, " gap "); g != nullptr; g = strstr(g + 1, " gap ")) {   // "m<N> gap …"
    const char *d = g;
    while (d > m && d[-1] >= '0' && d[-1] <= '9') d--;
    if (d < g && d > m && d[-1] == 'm') return "diag";
  }
  {   // "ota: …" at the start or after a space ("total:" must not match)
    const char *o = strstr(m, "ota:");
    if (o != nullptr && (o == m || o[-1] == ' ')) return "firmware";
  }
  for (const auto &k : KINDS) {
    if (strstr(m, k.needle) != nullptr) {
      if (!strcmp(k.kind, "node")) return strstr(m, " offline") ? "node_offline" : "node_online";
      return k.kind;
    }
  }
  return fail ? "fail" : "event";
}
static constexpr size_t REPLY_INLINE_MAX = 4000;   // ESPHome's JSON builder caps a response at 5119 bytes
static constexpr size_t PART_RAW = 3072;           // → 4096 base64 characters per part

void RBgridComponent::version_kick() {
  if (s_version_task != nullptr) xTaskNotifyGive(s_version_task);
}

static std::string b64(const char *p, size_t n) {
  static const char *const T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o;
  o.reserve((n + 2) / 3 * 4);
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint8_t) p[i] << 16;
    if (i + 1 < n) v |= (uint8_t) p[i + 1] << 8;
    if (i + 2 < n) v |= (uint8_t) p[i + 2];
    o += T[(v >> 18) & 63];
    o += T[(v >> 12) & 63];
    o += i + 1 < n ? T[(v >> 6) & 63] : '=';
    o += i + 2 < n ? T[v & 63] : '=';
  }
  return o;
}

// Config version off the loop task: the core builds the points + tariff JSON for it (a few KB, tens of ms).
static void version_task(void *) {
  for (;;) {
    if (s_core_up.load()) {
      char v[9];
      rbgrid_core_config_version(v);
      memcpy(RBgridComponent::version_buf, v, sizeof(v));
      RBgridComponent::version_ready.store(true);
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000));   // every 30 s, or at once after a changing command
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
  if (s_ev_q != nullptr) rbevlog_set_listener(on_event_line);
  vTaskDelete(nullptr);
}

void RBgridComponent::setup() {
  this->started_ms_ = millis();
  rbdynprice_set_ha_present(true);   // this build talks to Home Assistant: offer it as a price provider
  if (this->events_) s_ev_q = xQueueCreate(16, sizeof(EvItem));
  if (xTaskCreatePinnedToCore(start_task, "rbgrid_start", 12288, nullptr, 2, nullptr, 0) != pdPASS) {
    ESP_LOGE(TAG, "could not create the start task");
    this->mark_failed();
    return;
  }
  if (this->config_version_ != nullptr)
    xTaskCreatePinnedToCore(version_task, "rbgrid_ver", 6144, nullptr, 1, &s_version_task, 0);
}

// ESPHome restarts the chip itself (OTA, restart button/action): leave the same state behind as a planned restart.
void RBgridComponent::on_shutdown() {
  if (s_core_up.load()) rbgrid_core_prepare_restart();
}

void RBgridComponent::loop() {
  // A deferred stats migration is released only once the image can no longer roll back: ESPHome confirms the
  // OTA image itself (safe_mode), so wait for that, checked every 5 s after the grace period.
  if (!this->confirmed_ && millis() - this->started_ms_ > CONFIRM_AFTER_MS && millis() - this->confirm_check_ms_ > 5000) {
    this->confirm_check_ms_ = millis();
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run == nullptr || esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) {
      this->confirmed_ = true;
      rbgrid_core_image_confirmed();
    }
  }
  if (frame_ready.exchange(false)) this->publish_frame_();
  if (s_ev_q != nullptr) {
    EvItem it;
    for (int i = 0; i < 4 && xQueueReceive(s_ev_q, &it, 0) == pdTRUE; i++) this->fire_event_(it.msg, it.fail, it.seq);
  }
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
  // prices rounded to 5 decimals (the float32 transport still shows ~1e-8 noise beyond that)
  pub_(this->buy_price_, (float) (std::round(tn.buy_rate_all_in * 1e5) / 1e5));   // basis of the cost figures (fees + VAT)
  pub_(this->sell_price_, (float) (std::round((double) tn.sell_rate * 1e5) / 1e5));
  if (this->tariff_zone_ != nullptr && this->tariff_zone_->state != tn.zone) this->tariff_zone_->publish_state(tn.zone);
  if (this->time_valid_ != nullptr) this->time_valid_->publish_state(tn.time_valid);
  if (this->tariff_placeholder_ != nullptr) this->tariff_placeholder_->publish_state(tn.placeholder);
  if (this->external_price_ != nullptr) this->external_price_->publish_state(tn.external);
  if (this->price_state_ != nullptr && this->price_state_->state != tn.price_state)
    this->price_state_->publish_state(tn.price_state);
  if (this->price_source_ != nullptr && this->price_source_->state != tn.price_source)
    this->price_source_->publish_state(tn.price_source);
  if (this->price_valid_until_ != nullptr) {
    char iso[24] = "";
    if (tn.price_valid_until) {
      time_t vu = (time_t) tn.price_valid_until;
      struct tm g;
      gmtime_r(&vu, &g);
      strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &g);
    }
    if (this->price_valid_until_->state != iso) this->price_valid_until_->publish_state(iso);
  }
  if (this->provider_price_ != nullptr) {
    if (tn.external) pub_(this->provider_price_, (float) (std::round((double) tn.buy_rate * 1e5) / 1e5));
    else if (!std::isnan(this->provider_price_->state)) this->provider_price_->publish_state(NAN);
  }
  if (!this->currency_warned_ && tn.currency[0] && this->currency_ != tn.currency) {
    this->currency_warned_ = true;   // the money unit is fixed at build time; HA only labels it
    ESP_LOGW(TAG, "tariff currency is %s but the money entities are labelled %s (set `currency:` in YAML)", tn.currency,
             this->currency_.c_str());
  }
  if (this->modules_total_ || this->modules_online_) {
    int on = 0, tot = rbamp_bridge_module_counts(&on);
    if (this->modules_total_ && this->modules_total_->state != tot) this->modules_total_->publish_state(tot);
    if (this->modules_online_ && this->modules_online_->state != on) this->modules_online_->publish_state(on);
  }
  if (this->nodes_total_ || this->nodes_online_) {
    int tot = 0, on = 0;
    esp_now_hub_node_counts(&tot, &on);
    if (this->nodes_total_ && this->nodes_total_->state != tot) this->nodes_total_->publish_state(tot);
    if (this->nodes_online_ && this->nodes_online_->state != on) this->nodes_online_->publish_state(on);
  }
  if (this->slots_.empty()) return;

  if (!this->map_loaded_) this->load_map_();
  bool changed = false;
  // Free the slots of points that are gone.
  for (auto &sl : this->slots_) {
    if (sl.unit == 0) continue;
    bool found = false;
    for (int k = 0; k < n && !found; k++) found = pts[k].unit_id == sl.unit && !esp_now_hub_unit_switched_off(sl.unit);
    if (!found) { sl.unit = 0; sl.published_empty = false; changed = true; }
  }
  // Give new points the first free slot, in point-list order.
  for (int k = 0; k < n; k++) {
    bool mapped = false;
    for (auto &sl : this->slots_) if (sl.unit == pts[k].unit_id) { mapped = true; break; }
    if (mapped || esp_now_hub_unit_switched_off(pts[k].unit_id)) continue;   // switched off by the user: no slot
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
        pub_(sl.cost, NAN);
        if (sl.online != nullptr) sl.online->invalidate_state();
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
    pub_(sl.cost, (float) pt->cost_total);
    if (sl.online != nullptr && (!sl.online->has_state() || sl.online->state != pt->present))
      sl.online->publish_state(pt->present);
  }
}

std::string RBgridComponent::command(const std::string &json) {
  char *r = nullptr;
  {   // {"cmd":"get","section":"events","since":seq,"limit":50,"level":"all|event|fail"}: the device's event log
    cJSON *q = cJSON_Parse(json.c_str());
    const char *c = q ? cJSON_GetStringValue(cJSON_GetObjectItem(q, "cmd")) : nullptr;
    const char *sec = q ? cJSON_GetStringValue(cJSON_GetObjectItem(q, "section")) : nullptr;
    if (c && sec && !strcmp(c, "get") && !strcmp(sec, "events")) {
      const cJSON *js = cJSON_GetObjectItem(q, "since"), *jl = cJSON_GetObjectItem(q, "limit");
      const char *lv = cJSON_GetStringValue(cJSON_GetObjectItem(q, "level"));
      int minlvl = (lv && !strcmp(lv, "fail")) ? 2 : (lv && !strcmp(lv, "all")) ? 0 : 1;
      r = rbevlog_json(cJSON_IsNumber(js) ? (uint32_t) js->valuedouble : 0, cJSON_IsNumber(jl) ? (uint32_t) jl->valuedouble : 50,
                       minlvl);
    }
    cJSON_Delete(q);
  }
  if (r == nullptr) r = rbgrid_command_dispatch(json.c_str(), (int) json.size());
  if (r == nullptr) return "{\"status\":\"error\",\"message\":\"no reply\"}";
  std::string out(r);
  free(r);
  return out;
}

// The device reply decides success: an error ack (data.result "error"), a flat {"status":"error"} or an HTTP-style
// code >= 400 from a point operation. The message comes back as the action's error.
void RBgridComponent::run_command(const std::string &json) {
  this->last_ok_ = true;
  this->last_error_.clear();
  this->part_req_ = -1;
  cJSON *q = cJSON_Parse(json.c_str());
  const char *cmd = q ? cJSON_GetStringValue(cJSON_GetObjectItem(q, "cmd")) : nullptr;
  if (cmd != nullptr && !strcmp(cmd, "part")) {   // next part of a large reply, from the cache
    const cJSON *rid = cJSON_GetObjectItem(q, "rid"), *pt = cJSON_GetObjectItem(q, "part");
    size_t parts = (this->cache_.size() + PART_RAW - 1) / PART_RAW;
    if (this->cache_.empty() || millis() - this->cache_ms_ > 120000 || !cJSON_IsNumber(rid) ||
        (uint32_t) rid->valuedouble != this->cache_rid_ || !cJSON_IsNumber(pt) || pt->valueint < 0 ||
        (size_t) pt->valueint >= parts) {
      this->last_ok_ = false;
      this->last_error_ = "no such part (expired or another command ran since: repeat the command)";
    } else {
      this->part_req_ = pt->valueint;
    }
    cJSON_Delete(q);
    return;
  }
  bool changing = cmd != nullptr && strcmp(cmd, "get") != 0 && strcmp(cmd, "history") != 0;
  cJSON_Delete(q);
  this->cache_.clear();
  this->cache_.shrink_to_fit();
  // a config save / rule evaluation can block the loop task for seconds (flash): keep the task watchdog out of it
  const bool wdt_sub = esp_task_wdt_status(nullptr) == ESP_OK;
  if (wdt_sub) esp_task_wdt_delete(nullptr);
  this->last_reply_ = this->command(json);
  if (wdt_sub) esp_task_wdt_add(nullptr);
  if (changing) version_kick();
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
    // HA only receives the error text, so the machine-readable parts go in front of it:
    // "[field=zone][code=ZONE_NOT_OFFERED] message" (each prefix only when the reply has it; code = string codes only)
    const char *f = d ? cJSON_GetStringValue(cJSON_GetObjectItem(d, "field")) : nullptr;
    const char *c = d ? cJSON_GetStringValue(cJSON_GetObjectItem(d, "code")) : nullptr;
    if (c == nullptr) c = cJSON_GetStringValue(cJSON_GetObjectItem(r, "code"));
    std::string pre;
    if (f != nullptr && f[0]) pre += std::string("[field=") + f + "]";
    if (c != nullptr && c[0]) pre += std::string("[code=") + c + "]";
    if (!pre.empty()) this->last_error_ = pre + " " + this->last_error_;
  }
  cJSON_Delete(r);
}

void RBgridComponent::fill_reply(JsonObject root) {
  root["ok"] = this->last_ok_;
  root["schema"] = 1;
  root["config_version"] = this->version_;
  if (this->part_req_ >= 0) {   // a cached part
    size_t parts = (this->cache_.size() + PART_RAW - 1) / PART_RAW, off = (size_t) this->part_req_ * PART_RAW;
    root["rid"] = this->cache_rid_;
    root["parts"] = parts;
    root["part"] = this->part_req_;
    root["bytes"] = this->cache_.size();
    root["b64"] = b64(this->cache_.data() + off, std::min(PART_RAW, this->cache_.size() - off));
    this->cache_ms_ = millis();
    this->part_req_ = -1;
    return;
  }
  if (this->last_reply_.size() > REPLY_INLINE_MAX) {   // too large for one response: part 0 now, the rest on request
    this->cache_ = std::move(this->last_reply_);
    this->last_reply_.clear();
    this->cache_rid_ = this->cache_rid_ + 1 + (esp_random() & 0xFFFF);
    this->cache_ms_ = millis();
    size_t parts = (this->cache_.size() + PART_RAW - 1) / PART_RAW;
    root["rid"] = this->cache_rid_;
    root["parts"] = parts;
    root["part"] = 0;
    root["bytes"] = this->cache_.size();
    {   // the device reply's type, as a plain reply carries it
      const char *t = strstr(this->cache_.c_str(), "\"type\":\"");
      if (t != nullptr && t - this->cache_.c_str() < 256) {
        t += 8;
        const char *e = strchr(t, '"');
        if (e != nullptr && e - t < 40) root["type"] = std::string(t, e - t);
      }
    }
    root["b64"] = b64(this->cache_.data(), std::min(PART_RAW, this->cache_.size()));
    return;
  }
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

// One HA event "esphome.rbgrid_event" {kind, level, message}. Sent only while HA is connected (the device keeps the
// line in its own event log either way).
void RBgridComponent::fire_event_(const char *msg, bool fail, uint32_t seq) {
#ifdef USE_API
  if (api::global_api_server == nullptr || !api::global_api_server->is_connected()) return;
  api::HomeassistantActionRequest req;
  req.service = StringRef("esphome.rbgrid_event");
  req.is_event = true;
  const char *kind = event_kind(msg, fail);
  req.data.init(4);
  auto &k = req.data.emplace_back();
  k.key = StringRef("kind");
  k.value = StringRef(kind);
  auto &l = req.data.emplace_back();
  l.key = StringRef("level");
  // a node the user disabled is not a failure; diag lines are detail
  l.value = StringRef(!strcmp(kind, "node_disabled") ? "event" : fail ? "fail" : (!strcmp(kind, "diag") ? "debug" : "event"));
  auto &m = req.data.emplace_back();
  m.key = StringRef("message");
  m.value = StringRef(msg);
  char seqs[12];
  snprintf(seqs, sizeof(seqs), "%lu", (unsigned long) seq);
  auto &q = req.data.emplace_back();
  q.key = StringRef("seq");
  q.value = StringRef(seqs);
  api::global_api_server->send_homeassistant_action(req);
#endif
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
