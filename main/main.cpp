/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file main.cpp
 * @brief RBgrid application entry point: boot sequence, configuration, Wi-Fi/web/MQTT start-up, the rbAmp
 *        metering bridge and the debug console verbs.
 *
 * app_main() brings up NVS, storage, the rbpower home network from the stored configuration, the rbAmp I2C
 * fleet (main/rbamp_bridge.c), Wi-Fi, the web server, telemetry, MQTT and Home Assistant discovery, and the
 * optional ESP-NOW hub. Debug builds (CONFIG_RBGRID_DEBUG_BRIDGE or CONFIG_RBGRID_BENCH_FULL) also register
 * the rbbench console verbs defined in this file; they run in the rbbench console task (UART or
 * POST /api/v1/debug/cmd).
 */

// ===== BUILD FEATURE FLAGS (must precede the includes that use them) =====
// 🔴 EXPLICIT sdkconfig.h include — the `-include sdkconfig.h` force-include is NOT reliably
// in effect this early (same trap that silently flipped CONFIG_SPIRAM/MAX_UNITS off before).
// Without this, CONFIG_RBGRID_BENCH_FULL reads as undefined here → RBGRID_RBBENCH=0 → the whole
// rbbench console is compiled OUT of the unified build (dead console + no diag).
#include "sdkconfig.h"
// CONFIG_RBGRID_DEBUG_BRIDGE / CONFIG_RBGRID_BENCH_FULL come from sdkconfig.h (included above).
// - RBAMP_BRIDGE_ENABLE / RBGRID_RBAMP_FLEET: rbAmp I2C fleet as the telemetry source.
// - RBGRID_RBBENCH: rbbench console (load/meter/i2cscan verbs) — test-rig control.
// - RBGRID_WIFI: wifi + webserver + telemetry + MQTT.
// DECOUPLED so rbbench and wifi COEXIST in ONE firmware (PSRAM removed the RAM limit that forced
// them apart). RBGRID_BENCH_FULL=1 → the non-debug (wifi) build ALSO gets rbbench, so a load
// cross-check can be automated via the `load` verb in the MQTT build.
// Needs CONSOLE_NONE in sdkconfig (rbbench owns UART0; app ESP_LOG still prints via ROM). The
// pure debug-bridge build is unchanged (wifi off). A product build sets RBGRID_BENCH_FULL=0.
#ifndef RBAMP_BRIDGE_ENABLE
/** @brief 1 = the rbAmp I2C fleet is the metering source (default). */
#define RBAMP_BRIDGE_ENABLE 1
#endif
// RBGRID_BENCH_FULL mirrors the Kconfig CONFIG_RBGRID_BENCH_FULL (single source of truth so the
// CMake component gate and the C feature gate agree). A -D override on the command line still wins.
#ifndef RBGRID_BENCH_FULL
#if defined(CONFIG_RBGRID_BENCH_FULL)
/** @brief 1 = full debug build: the Wi-Fi build also gets the rbbench console (mirrors CONFIG_RBGRID_BENCH_FULL). */
#define RBGRID_BENCH_FULL 1
#else
/** @brief 0 = release build (no rbbench console in the Wi-Fi build). */
#define RBGRID_BENCH_FULL 0
#endif
#endif
#if defined(CONFIG_RBGRID_DEBUG_BRIDGE) || RBAMP_BRIDGE_ENABLE
/** @brief 1 = start the rbAmp fleet bridge. */
#define RBGRID_RBAMP_FLEET 1
#else
/** @brief 0 = rbAmp fleet bridge not started. */
#define RBGRID_RBAMP_FLEET 0
#endif
#if defined(CONFIG_RBGRID_DEBUG_BRIDGE) || RBGRID_BENCH_FULL
/** @brief 1 = compile in the rbbench console and the debug verbs of this file. */
#define RBGRID_RBBENCH 1
#else
/** @brief 0 = no rbbench console / debug verbs. */
#define RBGRID_RBBENCH 0
#endif
#if defined(CONFIG_RBGRID_DEBUG_BRIDGE)
/** @brief 1 = Wi-Fi, web server, telemetry and MQTT (0 in the pure debug-bridge build). */
#define RBGRID_WIFI 0
#else
/** @brief 1 = Wi-Fi, web server, telemetry and MQTT enabled. */
#define RBGRID_WIFI 1
#endif

#include <math.h>
#include <string.h>
#include <stdio.h>
#if RBGRID_BENCH_KIT
#include "bench_kit.h"   /* test-rig variant: built-in load control + YP9830 reference meter (one ESP32) */
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rbpower.h"
#include "rbstats_migrate.h"       // deferred stats-format migration: image-valid hook
#include "rbhome_tariff.h"   // rbpower_tariff_get_current_plan — provisioning-status tariff_set
#include "rbpower_inject_test.h"   // external-source injection self-test (opt-in)
#include "rbpower_logic_test.h"    // deterministic logic regression suite (selftest verb)
#include "rbamp_bridge.h"          // rbAmp I2C metering bridge
#include "esp_now_hub.h"           // ESP-NOW hub transport (beacon master + node recv)
#if RBGRID_RBBENCH
#include "rbbench.h"               // HW debug-bridge console (test harness)
#endif
#include "rbevlog.h"               // on-hub event log + remote console verbs (for OTA-only hubs)
extern "C" void modfw_register_http(void);   // main/modfw.c — STD module firmware update over the hub
extern "C" void rbi2c_register_http(void);   // main/rbi2c_api.c — I2C buses / module address API
#include "esp_system.h"
#include "rbhome_types.h"          // rbpower_get_unit_200ms_data for the `unit` verb
#include "esp_task_wdt.h"
/* Task-watchdog ISR hook: with the ESP console at NONE the TWDT panic print is lost; stash the task
 * running on each core in the RTC boot marker so the boot line names it. */
static void rbgrid_twdt_msg(void *opaque, const char *msg) { (void)opaque; rbpower_boot_note_wdt_msg(msg); }
extern "C" void esp_task_wdt_isr_user_handler(void)
{
    TaskHandle_t t0 = xTaskGetCurrentTaskHandleForCore(0);
    TaskHandle_t t1 = xTaskGetCurrentTaskHandleForCore(1);
    rbpower_boot_note_wdt(t0 ? pcTaskGetName(t0) : NULL, t1 ? pcTaskGetName(t1) : NULL);
    /* The running task is often just IDLE: the entry that STARVED is what matters —
     * walk the TWDT entry list the same way the IDF panic print does and stash the names too. */
    int cpus_fail = 0;
    esp_task_wdt_print_triggered_tasks(rbgrid_twdt_msg, NULL, &cpus_fail);
}
#include "rbpower_lifetime_api.h"  // etstat verb: et totalizers + restore-src
#include "mdns.h"                  // mDNS discovery
#include "esp_mac.h"               // esp_read_mac — mdns hostname rbgrid-<mac4>
#include "esp_app_desc.h"          // fw version in the _rbgrid._tcp TXT record
#include "rbtelemetry.h"
#include "rbmqtt.h"
#include "esp_mac.h"
#include "esp_wifi.h"   // esp_wifi_disconnect() — `wifidrop` debug verb
#include "esp_ota_ops.h" // OTA rollback: mark the freshly-OTA'd image valid once healthy
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"   // ha_discovery task + signal semaphore
#include "driver/i2c_master.h"   /* test-rig load-bank relay fail-safe OFF at boot */
#include "rbjson.h"
#include "rbwifi.h"
#include "nvs_flash.h"   /* debug-bridge build inits NVS here (rbwifi_init is gated out) */
#include "nvs.h"
#include "esp_random.h"
#include "esp_event.h"
#include "rbwebserver.h"
/*  @brief Add the web server's CORS headers to a response (rbwebserver CORS policy; its header is component-private).
 *  @param[in] req  HTTP request. */
extern "C" void add_cors_headers(httpd_req_t *req);  // rbwebserver CORS policy (internal header is component-private)
#include "rbwebsocket.h"
#include "rbwebsocket_telemetry.h"
#include "rbgrid.h"
#include "rbgrid_config_api.h"  // rbgrid_config_create and related config API
#include "rbgrid_config_storage.h"  // For config storage on external Flash
#include "littlefs_raw.h"           // littlefs_raw_remove — rbampcfg clear
#include "littlefs_raw.h"  // For raw file access

// Flash modules test
#include "rbextflash_config.h"
#include "rbextflash_config_runtime.h"
#include "rbextflash_operations.h"
#include "rbstats_v4_types.h"     // statmeta: raw v4 record view (set_meta check)
#include "rbstorage_transfer_types.h"   /* rbpower_electrical_transfer_t — stattest known-record */
#include "external_flash_config.h"

//#include "rbpower_console.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_private/esp_clk.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"

// ===== ADD FOR CONSOLE =====
#include "esp_console.h"
#include "esp_vfs_dev.h"
#include "esp_vfs.h"
#include "driver/uart_vfs.h"
#include "driver/uart.h"
#include "driver/uart_select.h"
#include "linenoise/linenoise.h"
#include "rbstrutil.h"     /* rb_scat — bounded snprintf append */

// ===== WI-FI removed - now in rbwifi module

// WiFi credentials: provisioned via a GITIGNORED local header so the secret never enters
// the tracked repo. Copy main/wifi_secrets.h.example -> main/wifi_secrets.h and fill it in.
// If the file is absent, the firmware falls back to NVS-stored credentials (AP portal).
#if defined(__has_include)
#  if __has_include("wifi_secrets.h")
#    include "wifi_secrets.h"
#  endif
#endif

// MQTT broker URI for the telemetry MQTT channel. Overridable in wifi_secrets.h or with -D. The default
// is "" (MQTT stays off until the broker is configured via /api/system/mqtt-config; rbmqtt treats empty
// as no-op). A persisted mqtt-config always wins.
#ifndef RBGRID_MQTT_BROKER_URI
/** @brief Default MQTT broker URI: empty = MQTT off until configured. */
#  define RBGRID_MQTT_BROKER_URI ""
#endif



// CONFIG_* constants for console
#ifndef CONFIG_ESP_CONSOLE_UART_NUM
/** @brief Console UART fallback when the Kconfig option is absent. */
#define CONFIG_ESP_CONSOLE_UART_NUM 0
#endif

/** @brief 1 = development default fleet (modules 0x50/0x51/0x52 with test CTs and a voltage trim for an
 *  uncalibrated sample); 0 (product) = one factory-calibrated module at 0x50, no host trim. Affects only a hub
 *  without a saved fleet config (/rbamp.json). */
#ifndef RBGRID_DEFAULT_BENCH_FLEET
#define RBGRID_DEFAULT_BENCH_FLEET 0
#endif

static const char* TAG = "RB-APP";

/* AP password: the setup password from provisioning (ap_provision.h, RBGRID_AP_SETUP_PASSWORD)
 * until the USER sets their own (POST /api/system/ap → NVS rbwifi/ap_pw_user). The hub never generates or changes it
 * by itself; a random per-device password stored by older firmware (NVS rbwifi/ap_pw) is dropped at the first boot. */
/* 
 * @brief Read the user-set AP password from NVS (rbwifi/ap_pw_user).
 * @param[out] out  Password buffer.
 * @param[in]  n    Size of @p out.
 * @return true if the user has set a password.
 */
extern "C" bool rbgrid_ap_user_password(char *out, size_t n);
/** Drop the hub-generated AP password stored by older firmware (NVS rbwifi/ap_pw). */
static void rbgrid_ap_drop_auto_password(void)
{
    nvs_handle_t h;
    esp_err_t oe = nvs_open("rbwifi", NVS_READWRITE, &h);
    if (oe == ESP_ERR_NVS_NOT_INITIALIZED && nvs_flash_init() == ESP_OK) oe = nvs_open("rbwifi", NVS_READWRITE, &h);
    if (oe != ESP_OK) return;
    if (nvs_erase_key(h, "ap_pw") == ESP_OK) {
        nvs_commit(h);
        rbpower_diag_printf("wifi: the hub-generated AP password is dropped — setup password until the user sets one");
    }
    nvs_close(h);
}

#if RBGRID_RBBENCH
/* RBgrid introspection verbs for the rbbench debug console.
 * Generic verbs (ping, i2c, load, meter) come from rbbench itself; these add
 * RBgrid-specific ones. Each verb replies through rbbench_reply() and returns 0 on success.
 * C linkage for the C function-pointer registry. */
extern "C" {
/** `heap`: free / minimum / largest heap figures (internal RAM and PSRAM). */
static int rbgrid_dbg_heap(int argc, char **argv) {
    rbbench_reply("heap free=%u min=%u | internal free=%u min=%u largest=%u | psram free=%u",
                  (unsigned)esp_get_free_heap_size(),
                  (unsigned)esp_get_minimum_free_heap_size(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return 0;
}
/** `shutdown`: planned restart after 200 ms. */
static int rbgrid_dbg_shutdown(int argc, char **argv) {
    /* TODO: flush storage/lifetime accumulators before reset once the shutdown
     * handler is wired — for now exercise the reboot path. */
    rbbench_reply("shutdown: restarting in 200ms");
    vTaskDelay(pdMS_TO_TICKS(200));
    { rbpower_mark_planned_restart(); esp_restart(); }
    return 0;
}
/** `reboot`: clean software restart (esp_restart, = the OTA reboot path). Reboots the hub without a re-flash
 * (an RTS pulse hangs these builds). */
static int rbgrid_dbg_reboot(int argc, char **argv) {
    rbbench_reply("reboot: esp_restart in 200ms");
    vTaskDelay(pdMS_TO_TICKS(200));
    { rbpower_mark_planned_restart(); esp_restart(); }
    return 0;
}

/* wifidrop — force a runtime STA disconnect to test auto-reconnect deterministically (a natural drop
 * can't be provoked on demand). Exercises exactly the runtime-drop path: the device
 * must self-heal (ever_connected → reconnect forever) WITHOUT a reboot → GOT_IP → esp-mqtt reconnect
 * → LWT online. `wifidrop` = immediate disconnect; the fix reconnects on its own. */
/** `phfeed`: test fixture — per-phase live data for a unit WITHOUT U3I3 hardware (for UI tests).
 * phfeed <hexid> <ph 1..3> <v> <i> <p> [q] [pf]  — upsert one phase entry (missing optional = omitted)
 * phfeed <hexid> clear                            — drop the unit's snapshot */
static int rbgrid_dbg_phfeed(int argc, char **argv) {
    if (argc < 3) { rbbench_reply("usage: phfeed <hexid> <ph 1..3> <v> <i> <p> [q] [pf] | phfeed <hexid> clear"); return 1; }
    uint16_t id = (uint16_t)strtoul(argv[1], NULL, 16);
    if (strcmp(argv[2], "clear") == 0) {
        rbpower_home_set_phase_live((rbpower_unit_id_t)id, NULL, 0);
        rbbench_reply("phfeed 0x%04X: cleared", (unsigned)id);
        return 0;
    }
    if (argc < 6) { rbbench_reply("usage: phfeed <hexid> <ph 1..3> <v> <i> <p> [q] [pf]"); return 1; }
    uint8_t ph = (uint8_t)strtoul(argv[2], NULL, 10);
    if (ph < 1 || ph > 3) { rbbench_reply("phfeed: ph must be 1..3"); return 1; }
    /* read-modify-write: keep the other phases' entries */
    rbpower_phase_live_t cur[3];
    int64_t age; uint8_t n = rbpower_home_get_phase_live((rbpower_unit_id_t)id, cur, &age);
    int idx = -1;
    for (uint8_t k = 0; k < n; k++) if (cur[k].ph == ph) { idx = k; break; }
    if (idx < 0) { if (n >= 3) { rbbench_reply("phfeed: 3 phases max"); return 1; } idx = n++; }
    cur[idx].ph = ph;
    cur[idx].v  = strtof(argv[3], NULL);
    cur[idx].i  = strtof(argv[4], NULL);
    cur[idx].p  = strtof(argv[5], NULL);
    cur[idx].q  = (argc > 6) ? strtof(argv[6], NULL) : NAN;
    cur[idx].pf = (argc > 7) ? strtof(argv[7], NULL) : NAN;
    cur[idx].angle = NAN; cur[idx].seq = 0;   /* test fixture: no angle / sequence */
    rbpower_err_t e = rbpower_home_set_phase_live((rbpower_unit_id_t)id, cur, n);
    rbbench_reply("phfeed 0x%04X ph%u: v=%.1f i=%.3f p=%.1f (%u entries) -> %d",
                  (unsigned)id, ph, cur[idx].v, cur[idx].i, cur[idx].p, n, (int)e);
    return 0;
}
/** `stdinfo <m>`: bring-up check — STD module identity (map/variant/n_v/n_i/period source/caps/status) +
 * the bridge's live per-phase V and Q cache. First thing to read with a U3I3 on the bus. */
static int rbgrid_dbg_stdinfo(int argc, char **argv) {
    uint8_t m = (argc > 1) ? (uint8_t)strtoul(argv[1], NULL, 10) : 0;
    rbamp_std_info_t si; float u[3], q[3]; uint8_t nv = 0, nc = 0;
    esp_err_t e = rbamp_bridge_std_info(m, &si, u, q, &nv, &nc);
    if (e != ESP_OK) { rbbench_reply("stdinfo m%u: %s (module absent or not STD line)", m, esp_err_to_name(e)); return 1; }
    rbbench_reply("stdinfo m%u: map=0x%02X variant=0x%02X n_v=%u n_i=%u period_src=%d(%s) caps=0x%04X status=0x%02X | bridge nv=%u nc=%u",
                  m, si.map_version, si.variant, si.n_voltage, si.n_current, (int)si.period_source,
                  si.period_source == 2 ? "counters" : si.period_source == 1 ? "latch" : "NONE",
                  (unsigned)si.capabilities, (unsigned)si.status, nv, nc);
    rbbench_reply("  live: U=%.1f/%.1f/%.1f V  Q=%.1f/%.1f/%.1f var", (double)u[0], (double)u[1], (double)u[2], (double)q[0], (double)q[1], (double)q[2]);
    return 0;
}
/** `wifidrop`: force a runtime STA disconnect (see the wifidrop note above). */
static int rbgrid_dbg_wifidrop(int argc, char **argv) {
    rbbench_reply("wifidrop: forcing STA disconnect — device should auto-reconnect (no reboot)");
    esp_err_t e = esp_wifi_disconnect();
    rbbench_reply("wifidrop: esp_wifi_disconnect() = %s", esp_err_to_name(e));
    return 0;
}
/** `wifistat`: WiFi diagnostics over the debug console (ESP_LOG is not on this UART, so network
 * failures are otherwise invisible). Prints mode, rbwifi status, STA IP/RSSI/channel, the saved
 * NVS SSID (never the password) and the AP netif state. `wifistat connect` re-issues esp_wifi_connect(). */
static int rbgrid_dbg_wifistat(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "connect") == 0) {
        esp_err_t e = esp_wifi_connect();
        rbbench_reply("wifistat: esp_wifi_connect() = %s", esp_err_to_name(e));
        return 0;
    }
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t me = esp_wifi_get_mode(&mode);
    rbwifi_info_t info = {};
    rbpower_err_t se = rbwifi_get_status(&info);
    char ssid[RBWIFI_MAX_SSID_LEN] = {0}, pass[RBWIFI_MAX_PASSWORD_LEN] = {0};
    bool have_creds = (rbwifi_nvs_load_credentials(ssid, pass) == RBPOWER_OK);
    rbbench_reply("wifistat: mode=%d(%s) status=%s nvs_ssid=%s",
                  (int)mode, (me == ESP_OK) ? "ok" : esp_err_to_name(me),
                  (se == RBPOWER_OK) ? rbwifi_status_to_string(info.status) : "getstatus-ERR",
                  have_creds ? ssid : "<none>");
    rbbench_reply("wifistat: sta ip=" IPSTR " gw=" IPSTR " rssi=%d ch=%u assoc_ssid=%.32s",
                  IP2STR(&info.ip_info.ip), IP2STR(&info.ip_info.gw),
                  (int)info.rssi, (unsigned)info.channel, (const char*)info.ap_info.ssid);
    esp_netif_t* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t apip = {};
    if (ap) esp_netif_get_ip_info(ap, &apip);
    wifi_config_t apcfg = {};
    esp_err_t ae = esp_wifi_get_config(WIFI_IF_AP, &apcfg);
    rbbench_reply("wifistat: ap ip=" IPSTR " ssid=%.32s ch=%u (%s)",
                  IP2STR(&apip.ip), (ae == ESP_OK) ? (const char*)apcfg.ap.ssid : "?",
                  (ae == ESP_OK) ? (unsigned)apcfg.ap.channel : 0u, esp_err_to_name(ae));
    return 0;
}
/** `unit <hexid>`: read back a unit's live 200 ms data — verifies the rbAmp->rbhome inject:
 * with a load applied, e.g. `unit 0006` (main supply) should reflect the rbAmp readings. */
static int rbgrid_dbg_unit(int argc, char **argv) {
    if (argc < 2) { rbbench_reply("usage: unit <hex_unit_id>  (e.g. unit 9010 or 0x9010)"); return 1; }
    /* unit IDs are hex by convention (0x9010 etc.) — parse base 16 (accepts an
     * optional 0x prefix), so `unit 9010` works as the user expects. */
    uint32_t id = (uint32_t)strtoul(argv[1], NULL, 16);
    if (id == 0 || id > 0xFFFF) {
        rbbench_reply("usage: unit <hex_unit_id>  (e.g. unit 9010 or 0x9010)");
        return 1;
    }
    rbpower_unit_realtime_average_200ms_t d;
    rbpower_err_t r = rbpower_get_unit_200ms_data((rbpower_unit_id_t)id, &d);
    if (r != RBPOWER_OK) {
        rbbench_reply("unit 0x%04X: err=%d", (unsigned)id, (int)r);
        return 1;
    }
    rbbench_reply("unit 0x%04X: U=%.1f I=%.3f P=%.1f PF=%.2f valid=%d hw_present=%d",
                  (unsigned)id,
                  (double)d.average_200ms.voltage_rms_avg,
                  (double)d.average_200ms.current_rms_avg,
                  (double)d.average_200ms.power_active_avg_w,
                  (double)d.average_200ms.power_factor_avg,
                  (int)d.is_valid,
                  (int)rbpower_get_unit_hardware_present((rbpower_unit_id_t)id));  // presence gate
    return 0;
}
/** `ctmodel <module> <ch> <code>`: CT model of one channel (per channel, unbound channels too), kept in the fleet
 * config. Writes the module only when the model differs — that replaces the channel's shipped calibration with the
 * preset, so the channel is marked the user's choice and re-applied after a module restart. */
static int rbgrid_dbg_ctmodel(int argc, char **argv) {
    uint32_t mod = 0, ch = 0, code = 0;
    if (argc < 4 || !rbbench_parse_uint(argv[1], &mod, 2) ||
        !rbbench_parse_uint(argv[2], &ch, 6) || !rbbench_parse_uint(argv[3], &code, 255)) {
        rbbench_reply("usage: ctmodel <module> <ch 0-6> <code> [factory]  (SCT-: 1=005 2=010 6=020 3=030 4=050 5=100; "
                      "factory = the model the module was calibrated for: number only, its calibration returns at `modreset`; "
                      "force = write the preset even when the number already matches, as the user's choice)");
        return 1;
    }
    int mode = argc >= 5 ? (!strcmp(argv[4], "factory") ? 1 : !strcmp(argv[4], "force") ? 2 : 0) : 0;
    char out[220];
    rbamp_bridge_ctmodel((uint8_t)mod, (uint8_t)ch, (uint8_t)code, mode, out, sizeof(out));
    rbbench_reply("%s", out);
    return 0;
}
/** `modreset <module>`: CMD_RESET of a module (it reloads its calibration from flash). */
static int rbgrid_dbg_modreset(int argc, char **argv) {
    uint32_t mod = 0;
    if (argc < 2 || !rbbench_parse_uint(argv[1], &mod, 2)) { rbbench_reply("usage: modreset <module>"); return 1; }
    char out[96];
    rbamp_bridge_modreset((uint8_t)mod, out, sizeof(out));
    rbbench_reply("%s", out);
    return 0;
}
/** `ctget <module> <ch>`: read a channel's CT model code. */
static int rbgrid_dbg_ctget(int argc, char **argv) {
    uint32_t mod = 0, ch = 0;
    if (argc < 3 || !rbbench_parse_uint(argv[1], &mod, 2) || !rbbench_parse_uint(argv[2], &ch, 3)) {
        rbbench_reply("usage: ctget <module> <ch>"); return 1;
    }
    uint8_t code = 0;
    esp_err_t e = rbamp_bridge_read_ct_model((uint8_t)mod, (uint8_t)ch, &code);
    if (e != ESP_OK) { rbbench_reply("ctget m=%u ch=%u: %s", (unsigned)mod, (unsigned)ch, esp_err_to_name(e)); return 1; }
    rbbench_reply("ctget m=%u ch=%u code=%u", (unsigned)mod, (unsigned)ch, code);
    return 0;
}
/** `ctsave <module>`: persist the module's user config (CT models) to its flash. */
static int rbgrid_dbg_ctsave(int argc, char **argv) {
    uint32_t mod = 0;
    if (argc < 2 || !rbbench_parse_uint(argv[1], &mod, 2)) { rbbench_reply("usage: ctsave <module>"); return 1; }
    esp_err_t e = rbamp_bridge_save_user_config((uint8_t)mod);
    rbbench_reply("ctsave m=%u: %s", (unsigned)mod, esp_err_to_name(e));
    return e == ESP_OK ? 0 : 1;
}
/** `period <module>`: latch the module's on-device period stats and inject the finished
 * RESULT (avg W / peak W / energy Wh) into the 15-min slot, bypassing the 200ms
 * accumulator. On demand for testing (no waiting 15 min); normal operation uses the quarter timer. */
static int rbgrid_dbg_period(int argc, char **argv) {
    uint32_t mod = 0;
    if (argc < 2 || !rbbench_parse_uint(argv[1], &mod, RBAMP_MAX_MODULES - 1)) {
        rbbench_reply("usage: period <module>  (latch+inject 15min period result)");
        return 1;
    }
    esp_err_t e = rbamp_bridge_collect_period((uint8_t)mod, RBPOWER_STAT_PERIOD_15MIN);
    rbbench_reply("period m=%u: %s (see log for per-unit avg/E)", (unsigned)mod, esp_err_to_name(e));
    return e == ESP_OK ? 0 : 1;
}
/** `pstat <hex_unit_id> [period]`: dump a unit's stored period RESULT (default 15min). */
static int rbgrid_dbg_pstat(int argc, char **argv) {
    if (argc < 2) { rbbench_reply("usage: pstat <hex_unit_id> [period]  (period: 1=15min default)"); return 1; }
    uint32_t id = (uint32_t)strtoul(argv[1], NULL, 16);
    uint32_t per = RBPOWER_STAT_PERIOD_15MIN;
    if (argc >= 3) (void)rbbench_parse_uint(argv[2], &per, RBPOWER_STAT_PERIOD_COUNT - 1);
    if (id == 0 || id > 0xFFFF) { rbbench_reply("usage: pstat <hex_unit_id> [period]"); return 1; }
    rbpower_home_unit_t *u = NULL;
    rbpower_err_t r = rbpower_get_unit_by_id((rbpower_unit_id_t)id, &u);
    if (r != RBPOWER_OK || u == NULL) { rbbench_reply("pstat 0x%04X: err=%d", (unsigned)id, (int)r); return 1; }
    const rbpower_period_statistics_t *p = &u->statistics.periods[per];
    rbbench_reply("pstat 0x%04X per=%lu: avgP=%.2fW maxP=%.2fW E_cons=%.4fWh E_net=%.4fWh dt=%lums complete=%d cost=%.4f rev=%.4f",
                  (unsigned)id, (unsigned long)per,
                  (double)p->power_active_avg, (double)p->extremes.power_max,
                  (double)p->energy_period.consumed_wh, (double)p->energy_period.net_wh,
                  (unsigned long)p->period_duration_ms, (int)p->period_complete,
                  (double)p->result_base.cost_consumed, (double)p->result_base.revenue_supplied);
    return 0;
}
/* `tariffdiag`: current (virtual in accelerated mode) calendar time + resolved active tariff zone & rates.
 * Lets a test watch zone transitions / weekend / holiday as the virtual clock moves. */
extern "C" void rbpower_tariff_get_diag(int*, int*, int*, int*, int*, float*, float*, int*);
static int rbgrid_dbg_tariffdiag(int argc, char **argv) {
    (void)argc; (void)argv;
    int h=0, wd=0, md=0, mo=0, z=0, hol=0; float buy=0, sell=0;
    rbpower_tariff_get_diag(&h, &wd, &md, &mo, &z, &buy, &sell, &hol);
    static const char* WD[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    rbbench_reply("tariffdiag vtime=%02d:00 %s %02d-%02d holiday=%d | active_zone=%d buy=%.3f sell=%.3f EUR/kWh",
                  h, (wd>=0&&wd<7)?WD[wd]:"?", mo, md, hol, z, (double)buy, (double)sell);
    return 0;
}
/* `tarifftime <unix_sec>`: freeze the tariff calendar at a chosen instant (or -1 to resume live clock)
 * so a test can jump to ANY scenario (weekend/holiday/peak/night) and verify the zone via tariffdiag.
 * Helper epochs (1970 UTC): 0=Thu Jan1 00:00 (holiday); 172800=Sat Jan3 00:00 (weekend);
 * 66600=Thu Jan1 18:30 (but Jan1 is holiday→use +2 days e.g. 238200=Sat... ) — pass any UNIX second. */
extern "C" void rbpower_tariff_set_test_time(long long unix_s);
static int rbgrid_dbg_tarifftime(int argc, char **argv) {
    if (argc < 2) { rbbench_reply("usage: tarifftime <unix_sec | -1 to resume live>"); return 1; }
    long long t = atoll(argv[1]);
    rbpower_tariff_set_test_time(t);
    rbbench_reply("tarifftime set=%lld (%s)", t, (t < 0) ? "live clock" : "frozen scenario");
    return 0;
}
/** `injtest <hexuid> <energy_wh signed> [avg_power_w]`: inject a SYNTHETIC 15-min period RESULT for
 * testing. Use a NEGATIVE energy_wh to validate feed-in / export REVENUE (which rbAmp consume-only
 * loads can't produce). Goes through the same inject+tariff-costing path → check `pstat <uid> 1`
 * cost=/rev=. Pair with `tarifftime` to test the sell rate in a specific zone. */
static int rbgrid_dbg_injtest(int argc, char **argv) {
    if (argc < 3) { rbbench_reply("usage: injtest <hexuid> <energy_wh signed> [avg_power_w]"); return 1; }
    uint32_t id = (uint32_t)strtoul(argv[1], NULL, 16);
    float e = (float)atof(argv[2]);
    float p = (argc >= 4) ? (float)atof(argv[3]) : (float)fabs(e) * 4.0f;   // rough P from a 15-min E
    float signed_p = (e < 0.0f) ? -p : p;
    float i = (p > 0.0f) ? p / 230.0f : 0.0f;
    rbpower_err_t r = rbpower_home_inject_period_result((rbpower_unit_id_t)id, RBPOWER_STAT_PERIOD_15MIN,
                                                        signed_p, signed_p, e, 900000u, 230.0f, i, i, i);
    rbbench_reply("injtest 0x%04X E=%.4fWh P=%.1fW rc=%d (now: pstat 0x%04X 1 → cost/rev)",
                  (unsigned)id, (double)e, (double)signed_p, (int)r, (unsigned)id);
    return 0;
}
/** `inj200 <hexuid> <v> <i> <p_signed> [q]`: inject ONE 200 ms live frame into a unit, exactly like the
 * rbAmp bridge / ESP-NOW hub do (rbpower_home_inject_200ms). Fixture for the live path: p_act/dir/pf
 * in telemetry, the direction label derived from the sign, sandbox units for UI tests on a hub without a
 * module. Does NOT write energy (energy_increment = 0, like the bridge). */
static int rbgrid_dbg_inj200(int argc, char **argv) {
    if (argc < 5) { rbbench_reply("usage: inj200 <hexuid> <v> <i> <p_signed> [q]"); return 1; }
    uint32_t id = (uint32_t)strtoul(argv[1], NULL, 16);
    float v = (float)atof(argv[2]), i = (float)atof(argv[3]), p = (float)atof(argv[4]);
    float q = (argc >= 6) ? (float)atof(argv[5]) : 0.0f;
    rbpower_unit_realtime_average_200ms_t m; memset(&m, 0, sizeof(m));
    m.average_200ms.samples_count          = 1;
    m.average_200ms.voltage_rms_avg        = v;
    m.average_200ms.current_rms_avg        = i;
    m.average_200ms.power_active_avg_w     = p;
    m.average_200ms.power_reactive_avg_var = q;
    m.average_200ms.power_apparent_avg_va  = v * i;
    m.average_200ms.power_factor_avg       = (v * i > 0.0f) ? (float)fabs(p) / (v * i) : 1.0f;
    m.is_valid = true;
    m.last_update_us = esp_timer_get_time();
    rbpower_err_t r = rbpower_home_inject_200ms((rbpower_unit_id_t)id, &m);
    if (r == RBPOWER_OK) rbpower_set_unit_hardware_present((rbpower_unit_id_t)id, true);   /* fixture = "sensor present" */
    rbbench_reply("inj200 0x%04X v=%.1f i=%.3f p=%.1f q=%.1f rc=%d", (unsigned)id, (double)v, (double)i, (double)p, (double)q, (int)r);
    return 0;
}
/* `zonestats <hexuid>`: per-zone cost + feed-in revenue breakdown (tariff statistics). Zone indices
 * shown: N=Night(0) D=Day(2) P=Peak(3) Wk=Weekend(5) Hol=Holiday(6). */
extern "C" void rbpower_tariff_get_zone_stats(rbpower_unit_id_t, float*, float*);
static int rbgrid_dbg_zonestats(int argc, char **argv) {
    if (argc < 2) { rbbench_reply("usage: zonestats <hexuid>"); return 1; }
    uint32_t id = (uint32_t)strtoul(argv[1], NULL, 16);
    float c[16] = {0}, r[16] = {0};
    rbpower_tariff_get_zone_stats((rbpower_unit_id_t)id, c, r);
    rbbench_reply("zonestats 0x%04X cost[N/D/P/Wk/Hol]=%.4f/%.4f/%.4f/%.4f/%.4f  rev=%.4f/%.4f/%.4f/%.4f/%.4f EUR",
                  (unsigned)id, (double)c[0],(double)c[2],(double)c[3],(double)c[5],(double)c[6],
                  (double)r[0],(double)r[2],(double)r[3],(double)r[5],(double)r[6]);
    return 0;
}
/* `netmeter <hexuid>`: net-metering state: this period's import/export + credit bank + lifetime. */
extern "C" void rbpower_tariff_get_net_metering(rbpower_unit_id_t, double*, double*, double*, double*, double*, double*);
extern "C" void rbpower_tariff_net_metering_settle(rbpower_unit_id_t);
static int rbgrid_dbg_netmeter(int argc, char **argv) {
    if (argc < 2) { rbbench_reply("usage: netmeter <hexuid>"); return 1; }
    uint32_t id = (uint32_t)strtoul(argv[1], NULL, 16);
    double pc=0, ps=0, bal=0, earn=0, used=0, billable=0;
    rbpower_tariff_get_net_metering((rbpower_unit_id_t)id, &pc, &ps, &bal, &earn, &used, &billable);
    rbbench_reply("netmeter 0x%04X period: import=%.4f export=%.4f kWh | credit_bal=%.4f | lifetime earned=%.4f used=%.4f | last_billable_net=%.4f kWh",
                  (unsigned)id, pc, ps, bal, earn, used, billable);
    return 0;
}
/* `lifetime <hexuid> | lifetime persist`: lifetime totalizers: precise et_in/et_out (mWh) + float totals,
 * and a forced NVS snapshot for the power-cycle restore test (flash -> feed -> persist -> power
 * cycle -> lifetime <uid> must show the persisted totals, NOT zeros). */
extern "C" rbpower_err_t rbpower_lifetime_get_totalizers(rbpower_unit_id_t, int64_t*, int64_t*);
extern "C" rbpower_err_t rbpower_lifetime_persist_to_nvs(void);
extern "C" rbpower_err_t rbpower_lifetime_get_unit_data(rbpower_unit_id_t, rbpower_lifetime_unit_data_t*);
static int rbgrid_dbg_lifetime(int argc, char **argv) {
    if (argc < 2) { rbbench_reply("usage: lifetime <hexuid> | lifetime persist"); return 1; }
    if (strcmp(argv[1], "persist") == 0) {
        rbpower_err_t e = rbpower_lifetime_persist_to_nvs();
        rbbench_reply("lifetime persist -> NVS: %s", (e == RBPOWER_OK) ? "OK" : "FAILED");
        return 0;
    }
    uint32_t id = (uint32_t)strtoul(argv[1], NULL, 16);
    int64_t et_in = 0, et_out = 0;
    rbpower_err_t e = rbpower_lifetime_get_totalizers((rbpower_unit_id_t)id, &et_in, &et_out);
    rbpower_lifetime_unit_data_t d = {};
    rbpower_lifetime_get_unit_data((rbpower_unit_id_t)id, &d);
    rbbench_reply("lifetime 0x%04X et_in=%lld mWh et_out=%lld mWh (%s) | float totals: in=%.1f out=%.1f Wh | daily: in=%.1f out=%.1f Wh",
                  (unsigned)id, (long long)et_in, (long long)et_out,
                  (e == RBPOWER_OK) ? "ok" : "ERR",
                  (double)d.total_energy_consumed_wh, (double)d.total_energy_supplied_wh,
                  (double)d.daily_energy_consumed_wh, (double)d.daily_energy_supplied_wh);
    return 0;
}
/* migdiag — trace of the last v3->v4 stats migration run (ESP_LOG is not visible on the debug console). */
extern "C" void rbstats_migrate_v3v4_diag(char *buf, size_t cap);
extern "C" void rbstats_migrate_v3v4_diag2(char *buf, size_t cap);
/** `statmeta <hex_unit> <flash_period 0=HOUR..4=YEAR> <dtype 0=elec 1=energy 2=econ> [n=2]`:
 * Raw v4 view of the NEWEST n records (through the v4 read shim, so on a v3 store the header
 * shows q=MIGRATED/dur=nominal/et=0). Exposes the write-path set_meta (dur/q/res/et_in/et_out)
 * that the history JSON does not serialize. */
static int rbgrid_dbg_statmeta(int argc, char **argv) {
    if (argc < 4) { rbbench_reply("usage: statmeta <hex_unit_id> <flash_period 0=HOUR..4=YEAR> <dtype 0=elec 1=energy 2=econ> [n]"); return 1; }
    uint32_t id = (uint32_t)strtoul(argv[1], NULL, 16);
    uint32_t per = 0, dt = 0, n = 2;
    rbbench_parse_uint(argv[2], &per, 8);
    rbbench_parse_uint(argv[3], &dt, 8);
    if (argc > 4) rbbench_parse_uint(argv[4], &n, 8);
    if (dt > RBPOWER_DATA_TYPE_ECONOMICS || per >= RBPOWER_PERIOD_MAX || n == 0) { rbbench_reply("statmeta: bad args"); return 1; }
    uint32_t total = 0;
    esp_err_t e = rbpower_flash_get_record_count((rbpower_unit_id_t)id, (rbpower_period_t)per, (rbpower_data_type_t)dt, &total);
    if (e != ESP_OK) { rbbench_reply("statmeta 0x%04X per=%u dt=%u: count %s", (unsigned)id, (unsigned)per, (unsigned)dt, esp_err_to_name(e)); return 1; }
    if (total == 0) { rbbench_reply("statmeta 0x%04X per=%u dt=%u: empty", (unsigned)id, (unsigned)per, (unsigned)dt); return 0; }
    if (n > total) n = total;
    if (n > 8) n = 8;
    size_t rs = (dt == RBPOWER_DATA_TYPE_ELECTRICAL) ? sizeof(rbstats_electrical_v4_t)
              : (dt == RBPOWER_DATA_TYPE_ENERGY)     ? sizeof(rbstats_energy_v4_t)
                                                     : sizeof(rbstats_econ_v4_t);
    uint8_t buf[8 * 43];
    uint32_t got = 0;
    /* read-path index 0 = NEWEST record (rbpower_flash_read_record counts back from the cursor) */
    e = rbstats_flash_read_v4((rbpower_unit_id_t)id, (rbpower_period_t)per, (rbpower_data_type_t)dt, 0, n, buf, &got);
    if (e != ESP_OK) { rbbench_reply("statmeta: read %s", esp_err_to_name(e)); return 1; }
    rbbench_reply("statmeta 0x%04X per=%u dt=%u total=%lu showing=%lu (idx 0 = newest)", (unsigned)id, (unsigned)per, (unsigned)dt, (unsigned long)total, (unsigned long)got);
    for (uint32_t k = 0; k < got; k++) {
        const uint8_t *r = buf + k * rs;
        rbstats_v4_hdr_t h; memcpy(&h, r, sizeof(h));
        if (dt == RBPOWER_DATA_TYPE_ENERGY) {
            rbstats_energy_v4_t v; memcpy(&v, r, sizeof(v));
            rbbench_reply("  [%lu] ts=%lu dur=%lu q=0x%04X res=%u ec_mwh=%lld es_mwh=%lld et_in_mwh=%lld et_out_mwh=%lld",
                          (unsigned long)k, (unsigned long)h.ts, (unsigned long)h.dur, (unsigned)h.q, (unsigned)h.res,
                          (long long)v.ec_mwh, (long long)v.es_mwh, (long long)v.et_in_mwh, (long long)v.et_out_mwh);
        } else if (dt == RBPOWER_DATA_TYPE_ECONOMICS) {
            rbstats_econ_v4_t v; memcpy(&v, r, sizeof(v));
            rbbench_reply("  [%lu] ts=%lu dur=%lu q=0x%04X res=%u c_cents=%lld r_cents=%lld plan=%u/%u",
                          (unsigned long)k, (unsigned long)h.ts, (unsigned long)h.dur, (unsigned)h.q, (unsigned)h.res,
                          (long long)v.c_cents, (long long)v.r_cents, (unsigned)v.plan_id, (unsigned)v.plan_ver);
        } else {
            rbstats_electrical_v4_t v; memcpy(&v, r, sizeof(v));
            rbbench_reply("  [%lu] ts=%lu dur=%lu q=0x%04X res=%u present=0x%03X v=%.1f i=%.3f p=%.1f pf=%.2f f=%u",
                          (unsigned long)k, (unsigned long)h.ts, (unsigned long)h.dur, (unsigned)h.q, (unsigned)h.res,
                          (unsigned)v.present, v.voltage_rms_v10 / 10.0, v.current_rms_ma / 1000.0, v.power_active_w10 / 10.0,
                          v.power_factor_100 / 100.0, (unsigned)v.frequency_hz10);
        }
    }
    return 0;
}

/** `migdiag`: print the trace of the last v3->v4 stats migration (two lines). */
static int rbgrid_dbg_migdiag(int argc, char **argv) {
    (void)argc; (void)argv;
    char buf[320];
    rbstats_migrate_v3v4_diag(buf, sizeof(buf));
    rbbench_reply("%s", buf);                /* line 1: slot map / totals (≤160 chars) */
    rbstats_migrate_v3v4_diag2(buf, sizeof(buf));
    rbbench_reply("%s", buf);                /* line 2: HOUR per-dtype trace of the last slot */
    return 0;
}
/** `netsettle <hexuid>`: manually trigger net-metering settlement (auto-runs at the month boundary). */
static int rbgrid_dbg_netsettle(int argc, char **argv) {
    if (argc < 2) { rbbench_reply("usage: netsettle <hexuid>"); return 1; }
    uint32_t id = (uint32_t)strtoul(argv[1], NULL, 16);
    rbpower_tariff_net_metering_settle((rbpower_unit_id_t)id);
    rbbench_reply("netsettle 0x%04X done (now: netmeter 0x%04X)", (unsigned)id, (unsigned)id);
    return 0;
}
/* progmode <0|1> — toggle the progressive (block-tier) pricing overlay (OFF by default, so the TOU/all-in
 * path is untouched until a tier test enables it). */
extern "C" void rbpower_tariff_set_progressive(bool);
extern "C" void rbpower_tariff_get_progressive(rbpower_unit_id_t, bool*, double*, int*, float*);
/** `progmode <0|1>`: toggle the progressive (block-tier) pricing overlay. */
static int rbgrid_dbg_progmode(int argc, char **argv) {
    if (argc < 2) { rbbench_reply("usage: progmode <0|1>"); return 1; }
    bool on = (strtoul(argv[1], NULL, 10) != 0);
    rbpower_tariff_set_progressive(on);
    rbbench_reply("progmode = %s", on ? "ON" : "OFF");
    return 0;
}
/** `progtier <hexuid>`: progressive state: cumulative monthly import + active tier + rate multiplier. */
static int rbgrid_dbg_progtier(int argc, char **argv) {
    if (argc < 2) { rbbench_reply("usage: progtier <hexuid>"); return 1; }
    uint32_t id = (uint32_t)strtoul(argv[1], NULL, 16);
    bool en = false; double cum = 0.0; int tier = 0; float mult = 1.0f;
    rbpower_tariff_get_progressive((rbpower_unit_id_t)id, &en, &cum, &tier, &mult);
    rbbench_reply("progtier 0x%04X: progressive=%s | cumulative_import=%.4f kWh | tier=%d | rate_mult=%.2fx",
                  (unsigned)id, en ? "ON" : "OFF", cum, tier, mult);
    return 0;
}
/* 4-flow source-aware energy balance scaffold (merit-order PV→load→battery→grid). */
extern "C" void rbpower_compute_energy_balance(const rbpower_energy_balance_input_t*, rbpower_energy_4flow_t*);
extern "C" void rbpower_gather_system_balance_input(rbpower_energy_balance_input_t*);
/** Print the input and the 4-flow result of an energy-balance run. */
static void rbgrid_print_flows(const rbpower_energy_balance_input_t* in, const rbpower_energy_4flow_t* f, const char* tag) {
    rbbench_reply("%s in: grid=%.1f pv=%.1f batt=%.1f load=%.1f W", tag, in->p_grid, in->p_pv, in->p_batt, in->p_load);
    rbbench_reply("  pv->load=%.1f pv->grid=%.1f chg<-pv=%.1f chg<-grid=%.1f W",
                  f->pv_to_load, f->pv_to_grid, f->charge_from_pv, f->charge_from_grid);
    rbbench_reply("  dis->load=%.1f dis->grid=%.1f grid->load=%.1f W | self_cons=%.0f%%",
                  f->discharge_to_load, f->discharge_to_grid, f->grid_to_load, f->self_consumption_ratio * 100.0f);
}
/** `enbalance <p_grid> <p_pv> <p_batt> <p_load>`: synthetic merit-order test (p_grid +imp/-exp, p_batt +chg/-dis). */
static int rbgrid_dbg_enbalance(int argc, char **argv) {
    if (argc < 5) { rbbench_reply("usage: enbalance <p_grid> <p_pv> <p_batt> <p_load>  (W; p_batt +charge/-discharge)"); return 1; }
    rbpower_energy_balance_input_t in = {};
    in.p_grid = strtof(argv[1], NULL); in.p_pv = strtof(argv[2], NULL);
    in.p_batt = strtof(argv[3], NULL); in.p_load = strtof(argv[4], NULL);
    rbpower_energy_4flow_t f; rbpower_compute_energy_balance(&in, &f);
    rbgrid_print_flows(&in, &f, "enbalance");
    return 0;
}
/** `sysflows`: gather the live system snapshot by role + run the balance (a grid-only system gives a no-op). */
static int rbgrid_dbg_sysflows(int argc, char **argv) {
    (void)argc; (void)argv;
    rbpower_energy_balance_input_t in; rbpower_gather_system_balance_input(&in);
    rbpower_energy_4flow_t f; rbpower_compute_energy_balance(&in, &f);
    rbgrid_print_flows(&in, &f, "sysflows");
    return 0;
}
/* fmtinfo — record-format compat state (boot migration ESP_LOG isn't visible on rbbench). Shows what
 * the next format change would do: steady / compat-upgrade (history preserved) / incompatible (wipe). */
extern "C" void rbpower_flash_get_format_info(uint32_t*, uint32_t*, uint32_t*, bool*);
/** `fmtinfo`: stats record-format compatibility state and what the next format change would do. */
static int rbgrid_dbg_fmtinfo(int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t cur = 0, minc = 0, stored = 0; bool present = false;
    rbpower_flash_get_format_info(&cur, &minc, &stored, &present);
    const char* verdict = !present ? "no marker (fresh/pre-versioning -> WIPE)"
        : (stored == cur) ? "OK (steady, no migration)"
        : (stored >= minc && stored < cur) ? "compat-upgrade (history PRESERVED)"
        : "INCOMPATIBLE (would WIPE)";
    if (present)
        rbbench_reply("fmtinfo: current=v%u min_compatible=v%u | stored_marker=v%u | %s",
                      (unsigned)cur, (unsigned)minc, (unsigned)stored, verdict);
    else
        rbbench_reply("fmtinfo: current=v%u min_compatible=v%u | stored_marker=(none) | %s",
                      (unsigned)cur, (unsigned)minc, verdict);
    return 0;
}
/* teletime [iters] — measure the telemetry-build CPU cost FULL vs LEAN. Builds the payload N
 * times in each profile, reports us/build + the saving. Restores the live profile afterward. */
extern "C" char* rbgrid_json_create_telemetry(void);
extern "C" void rbgrid_telemetry_set_lean(bool);
extern "C" bool rbgrid_telemetry_get_lean(void);
/** `teletime [iters]`: telemetry build time, full vs lean profile. */
static int rbgrid_dbg_teletime(int argc, char **argv) {
    int iters = (argc >= 2) ? atoi(argv[1]) : 30;
    if (iters < 1) iters = 30;
    bool saved = rbgrid_telemetry_get_lean();
    rbgrid_telemetry_set_lean(false);
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < iters; i++) { char* s = rbgrid_json_create_telemetry(); if (s) free(s); }
    int64_t full_us = (esp_timer_get_time() - t0) / iters;
    rbgrid_telemetry_set_lean(true);
    t0 = esp_timer_get_time();
    for (int i = 0; i < iters; i++) { char* s = rbgrid_json_create_telemetry(); if (s) free(s); }
    int64_t lean_us = (esp_timer_get_time() - t0) / iters;
    rbgrid_telemetry_set_lean(saved);
    rbbench_reply("teletime (%d iters): full=%lld us/build, lean=%lld us/build, saved=%lld us (%.0f%%) | live profile=%s",
                  iters, (long long)full_us, (long long)lean_us, (long long)(full_us - lean_us),
                  full_us > 0 ? 100.0 * (double)(full_us - lean_us) / (double)full_us : 0.0,
                  saved ? "lean" : "full");
    return 0;
}
/** `token [set <t> | clear]`: provision the API auth token. No token = auth OFF (writes open);
 * setting one enables Bearer/X-API-Key enforcement on all POST/DELETE. Console-only (local provisioning). */
static int rbgrid_dbg_token(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "set")) {
        littlefs_raw_write("/api_token", argv[2], strlen(argv[2]) + 1);
        rbbench_reply("token SET → auth ENFORCED on writes (clients send 'Authorization: Bearer %s')", argv[2]);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "clear")) {
        littlefs_raw_remove("/api_token");
        rbbench_reply("token CLEARED → auth OFF (writes open)");
        return 0;
    }
    char t[48] = {0}; size_t act = 0;
    bool has = littlefs_raw_exists("/api_token") &&
               littlefs_raw_read("/api_token", t, sizeof(t) - 1, &act) == ESP_OK && t[0];
    rbbench_reply("token: %s | auth %s | usage: token | token set <t> | token clear",
                  has ? t : "(none)", has ? "ENFORCED on writes" : "OFF (writes open)");
    return 0;
}
/** `calgain`: field-calibration host-gain trim for the rbAmp fleet. The trim is a
 * FALLBACK for an uncalibrated module — factory-calibrated rbAmp modules carry their gains in module
 * flash and should stay at 1.0 (RBgrid never writes module cal). See documents/development/ACCURACY_SPEC.md.
 *   calgain                 -> show live + persisted gains (×1000 as int; nano-printf omits %f)
 *   calgain v <factor>      -> set voltage gain (e.g. 0.974), apply live + persist /rbamp.json
 *   calgain i <factor>      -> set current/power gain, apply live + persist
 *   calgain reset           -> both gains to 1.0, apply live + persist
 * Procedure: measure DUT vs a reference meter, factor = reference / DUT, set, re-verify. */
static int rbgrid_dbg_calgain(int argc, char **argv) {
    float vlive = 1.0f, ilive = 1.0f;
    rbamp_bridge_get_host_gains(&vlive, &ilive);
    if (argc < 2) {
        rbamp_bridge_config_t pc; bool havep = (rbgrid_storage_load_rbamp(&pc) == ESP_OK);
        rbbench_reply("calgain: LIVE v=%d i=%d (×1000) | PERSISTED %s v=%d i=%d | usage: calgain v|i <factor> | calgain reset",
                      (int)(vlive * 1000.0f), (int)(ilive * 1000.0f),
                      havep ? "/rbamp.json" : "(none)",
                      havep ? (int)(pc.voltage_gain * 1000.0f) : 0,
                      havep ? (int)(pc.current_gain * 1000.0f) : 0);
        return 0;
    }
    float nv = vlive, ni = ilive;
    if (!strcmp(argv[1], "reset")) { nv = 1.0f; ni = 1.0f; }
    else if (argc >= 3 && (!strcmp(argv[1], "v") || !strcmp(argv[1], "i"))) {
        float f = (float)atof(argv[2]);
        if (!(f >= 0.5f && f <= 2.0f)) {   /* trim beyond ±50% means a wiring/CT-code error, not calibration */
            rbbench_reply("calgain: factor %d (×1000) out of sane band [500..2000]; refusing (check CT code/wiring)",
                          (int)(f * 1000.0f));
            return 1;
        }
        if (!strcmp(argv[1], "v")) nv = f; else ni = f;
    } else {
        rbbench_reply("calgain: usage: calgain | calgain v <factor> | calgain i <factor> | calgain reset");
        return 1;
    }
    esp_err_t e = rbamp_bridge_set_host_gains(nv, ni);   /* apply live */
    if (e != ESP_OK) { rbbench_reply("calgain: set_host_gains failed (%s)", esp_err_to_name(e)); return 1; }
    /* persist: load the saved fleet, patch the gains, save back (so it survives reboot). */
    rbamp_bridge_config_t cfg;
    if (rbgrid_storage_load_rbamp(&cfg) != ESP_OK) {
        rbbench_reply("calgain: applied LIVE (v=%d i=%d ×1000) but no /rbamp.json to persist — reboot saves default; re-run after",
                      (int)(nv * 1000.0f), (int)(ni * 1000.0f));
        return 0;
    }
    cfg.voltage_gain = nv; cfg.current_gain = ni;
    esp_err_t se = rbgrid_storage_save_rbamp(&cfg);
    rbbench_reply("calgain: SET live + persist %s -> v=%d i=%d (×1000)%s",
                  esp_err_to_name(se), (int)(nv * 1000.0f), (int)(ni * 1000.0f),
                  (nv == 1.0f && ni == 1.0f) ? " [unity — relying on module-flash cal]" : "");
    return (se == ESP_OK) ? 0 : 1;
}
/** `espnow [on [ch] | off | pair [s] | latch | nodes | addnode | delnode | disable | enable | status]`: ESP-NOW
 * hub control. `on` starts the beacon time master + node receiver; optional ch forces the WiFi/ESP-NOW channel
 * (nodes must match). */
static int rbgrid_dbg_espnow(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "on")) {
        uint8_t ch = (argc >= 3) ? (uint8_t)atoi(argv[2]) : 0;
        wifi_ap_record_t ap;
        bool wifi_ready = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);   /* hub channel = router channel */
        esp_err_t e = esp_now_hub_start(ch);
        bool run = esp_now_hub_running();
        /* report the ACTUAL running state, not just the call's return — early-after-boot WiFi-not-ready was a
         * recurring gotcha (start "ok" but running=0 → no beacons). Auto-start also brings it up when ready. */
        rbbench_reply("espnow on: %s, running=%d%s", esp_err_to_name(e), (int)run,
                      run ? " — beacon master active" : (wifi_ready ? "" : " — WiFi not ready yet, will auto-start when associated"));
        return run ? 0 : 1;
    }
    if (argc >= 2 && !strcmp(argv[1], "off")) { esp_now_hub_stop(); rbbench_reply("espnow off"); return 0; }
    if (argc >= 2 && !strcmp(argv[1], "pair")) {   /* open the over-the-air pairing window */
        if (!esp_now_hub_running()) { rbbench_reply("espnow pair: run `espnow on` first"); return 1; }
        uint32_t sec = (argc >= 3) ? (uint32_t)atoi(argv[2]) : 120;
        esp_now_hub_open_pairing(sec);
        rbbench_reply("espnow pair: window OPEN %us — an unpaired node's PAIR_REQ → registry. Then `espnow status` "
                      "shows paired=N; run `reboot` to register its units.", sec);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "latch")) {   /* command all nodes to latch their period now */
        if (!esp_now_hub_running()) { rbbench_reply("espnow latch: run `espnow on` first"); return 1; }
        uint32_t n = esp_now_hub_latch_all(0 /*manual*/);
        rbbench_reply("espnow latch: LATCH_NOW → %u node(s) (each closes its period now → PERIOD frame). "
                      "Auto-fires on tariff-zone transitions.", n);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "nodes")) {   /* list the persisted ESP-NOW node registry */
        espnow_registry_t reg; esp_now_registry_load(&reg);
        rbbench_reply("espnow registry: %d node(s)", reg.node_count);
        for (int i = 0; i < reg.node_count; i++) {
            const espnow_reg_node_t* nd = &reg.nodes[i];
            rbbench_reply("  [%d] %02X:%02X:%02X:%02X:%02X:%02X ch_count=%u (ch0=%u:ct%u)", i,
                          nd->mac[0],nd->mac[1],nd->mac[2],nd->mac[3],nd->mac[4],nd->mac[5],
                          nd->ch_count, nd->ch_count?nd->channels[0].ch:0, nd->ch_count?nd->channels[0].ct_code:0);
        }
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "addnode")) {   /* espnow addnode <mac> <ch:ct> [<ch:ct>...] */
        uint8_t mac[6];
        if (sscanf(argv[2], "%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx", &mac[0],&mac[1],&mac[2],&mac[3],&mac[4],&mac[5]) != 6 &&
            sscanf(argv[2], "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", &mac[0],&mac[1],&mac[2],&mac[3],&mac[4],&mac[5]) != 6) {
            rbbench_reply("espnow addnode: bad MAC (e8db843d42e8 or e8:db:84:3d:42:e8)"); return 1;
        }
        espnow_reg_chan_t ch[ESPNOW_REG_MAX_CH]; uint8_t n = 0;
        for (int i = 3; i < argc && n < ESPNOW_REG_MAX_CH; i++) {
            unsigned c, ct;
            if (sscanf(argv[i], "%u:%u", &c, &ct) == 2) { ch[n].ch = (uint8_t)c; ch[n].ct_code = (uint8_t)ct; n++; }
        }
        if (n == 0) { rbbench_reply("espnow addnode: need <ch>:<ct> (ct 2=10A 3=30A 4=50A 6=20A)"); return 1; }
        esp_err_t e = esp_now_registry_add_node(mac, ch, n);
        rbbench_reply("espnow addnode: %s (%u ch) — REBOOT to register units", esp_err_to_name(e), n);
        return (e == ESP_OK) ? 0 : 1;
    }
    if (argc >= 3 && !strcmp(argv[1], "delnode")) {       /* cascade — removes the node AND its units now */
        uint8_t mac[6];
        if (sscanf(argv[2], "%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx", &mac[0],&mac[1],&mac[2],&mac[3],&mac[4],&mac[5]) != 6 &&
            sscanf(argv[2], "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", &mac[0],&mac[1],&mac[2],&mac[3],&mac[4],&mac[5]) != 6) {
            rbbench_reply("espnow delnode: bad MAC"); return 1;
        }
        uint32_t rm = esp_now_hub_delnode_cascade(mac);
        rbbench_reply("espnow delnode: node + %u unit(s) removed (registry + units, no reboot needed)", rm);
        return 0;
    }
    if (argc >= 3 && (!strcmp(argv[1], "disable") || !strcmp(argv[1], "enable"))) {   /* silence/resume */
        bool dis = !strcmp(argv[1], "disable"); uint8_t mac[6];
        if (sscanf(argv[2], "%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx", &mac[0],&mac[1],&mac[2],&mac[3],&mac[4],&mac[5]) != 6 &&
            sscanf(argv[2], "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", &mac[0],&mac[1],&mac[2],&mac[3],&mac[4],&mac[5]) != 6) {
            rbbench_reply("espnow %s: bad MAC", argv[1]); return 1;
        }
        esp_now_hub_set_node_disabled(mac, dis);
        rbbench_reply("espnow %s: node %s (runtime — its data is %s)", argv[1], dis ? "DISABLED" : "ENABLED",
                      dis ? "ignored, units absent" : "flowing again");
        return 0;
    }
    /* report counters via rbbench_reply — ESP_LOG isn't on the hub console. */
    uint8_t ch = 0; uint32_t bs = 0, rt = 0, st = 0, uc = 0, pr = 0, pi = 0; float i0 = 0, i1 = 0, sd = 0;
    esp_now_hub_get_stats(&ch, &bs, &rt, &st, &i0, &i1, &sd, &uc, &pr, &pi);
    rbbench_reply("espnow: running=%d ch=%u pairing=%d paired=%u | beacons=%u rt_recv=%u stats=%u period_rx=%u period_inj=%u | node i0=%d i1=%d mA node_sd=%d us",
                  (int)esp_now_hub_running(), ch, (int)esp_now_hub_pairing_open(), (unsigned)esp_now_hub_paired_count(),
                  (unsigned)bs, (unsigned)rt, (unsigned)st, (unsigned)pr, (unsigned)pi,
                  (int)(i0 * 1000.0f), (int)(i1 * 1000.0f), (int)sd);
    return 0;
}
/* selftest — run the on-device deterministic logic regression suite
 * (rbpower_logic_run_tests: 4-flow merit-order attribution). Prints per-check PASS/FAIL to
 * ESP_LOG and a machine-parseable "RBTEST logic SUMMARY: <P> passed, <F> failed" + a verdict
 * to the console. Runtime — no rebuild. See documents/development/TEST_MATRIX.md. */
extern "C" const char *rbpower_logic_fail_name(int i);
/** `selftest`: run the deterministic logic regression suite; returns the failure count. */
static int rbgrid_dbg_selftest(int argc, char **argv) {
    int failures = rbpower_logic_run_tests();
    rbbench_reply("selftest: logic suite verdict = %s (%d failure%s)",
                  failures == 0 ? "PASS" : "FAIL", failures, failures == 1 ? "" : "s");
    /* ESP_LOG is invisible on the debug console — name every failing check in the reply. */
    for (int i = 0; ; i++) {
        const char *n = rbpower_logic_fail_name(i);
        if (!n) break;
        rbbench_reply("  FAIL[%d]: %s", i, n);
    }
    return failures;   /* nonzero rc = failures, for scripted parsing */
}
/** `webhttps [on|off]`: switch the web server between HTTP (port 80) and HTTPS/TLS (port 443).
 * Persisted to littlefs; takes effect on next REBOOT (httpd_start vs httpd_ssl_start differ). Cert:
 * prod /https_cert.pem+/https_key.pem if present, else embedded self-signed dev cert (use curl -k). */
static int rbgrid_dbg_webhttps(int argc, char **argv) {
    if (argc >= 2 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "off"))) {
        bool on = !strcmp(argv[1], "on");
        rbwebserver_set_https(on);
        rbbench_reply("webhttps %s → effective after REBOOT (port %d). Cert: %s",
                      on ? "ENABLED" : "disabled", on ? 443 : 80,
                      littlefs_raw_exists("/https_cert.pem") && littlefs_raw_exists("/https_key.pem")
                          ? "PROD (storage)" : "self-signed DEV (curl -k)");
        return 0;
    }
    rbbench_reply("webhttps: desired=%s, running=%s | usage: webhttps on | webhttps off (reboot to apply)",
                  rbwebserver_get_https() ? "HTTPS" : "HTTP",
                  rbwebserver_https_active() ? "HTTPS/TLS:443" : "HTTP:80");
    return 0;
}
/* Period-boundary hook: fired by the statistics task at each 15-min boundary. Collects
 * every rbAmp module's finished period into the 15-min slot (on-device for UI modules,
 * host-side V_bus×I for current-only) — synchronized with the rbpower statistics hierarchy. Higher
 * periods (1h/day) aggregate from the accumulated 15-min results separately. */
/** `status`: dump per-module variant/energy-path + heap (boot ESP_LOG is silent under
 * CONSOLE_NONE, so this gives the fleet/energy state via the console). */
static int rbgrid_dbg_status(int argc, char **argv) {
    static const char *emode[] = { "NONE", "ON-DEVICE", "HOST" };
    uint8_t shown = 0;
    for (uint8_t m = 0; m < RBAMP_MAX_MODULES; ++m) {
        uint8_t addr = 0, em = 0; bool hv = false;
        if (!rbamp_bridge_module_status(m, &addr, &hv, &em)) continue;
        rbbench_reply("module %u: 0x%02X voltage_hw=%s energy=%s",
                      (unsigned)m, (unsigned)addr, hv ? "yes" : "no", em < 3 ? emode[em] : "?");
        shown++;
    }
    rbbench_reply("modules=%u free_heap=%u", (unsigned)shown, (unsigned)esp_get_free_heap_size());
    return 0;
}

/** `rbampcfg show|clear|count|addr|phases`: inspect/edit the PERSISTED rbAmp fleet config (/rbamp.json).
 *   show  (default): load + print the saved fleet — proves the config survives reboot.
 *   clear         : delete the file so main.cpp re-saves its default fleet on the next boot.
 * Gains printed ×1000 as ints (nano-printf may omit %f). */
static int rbgrid_dbg_rbampcfg(int argc, char **argv) {
    const char *sub = (argc > 1) ? argv[1] : "show";
    if (strcmp(sub, "clear") == 0) {
        esp_err_t e = littlefs_raw_remove("/rbamp.json");
        rbbench_reply("rbampcfg clear: %s (reboot -> default re-saved)", esp_err_to_name(e));
        return (e == ESP_OK) ? 0 : 1;
    }
    rbamp_bridge_config_t cfg;
    esp_err_t e = rbgrid_storage_load_rbamp(&cfg);
    if (e != ESP_OK) {
        rbbench_reply("rbampcfg show: no saved config (%s)", esp_err_to_name(e));
        return 1;
    }
    /* Test binding: `rbampcfg phases <m> <hexunit> <ct>` — replace module m's CURRENT
     * entries with one grouped entry (all channels -> unit), keep its voltage entry, persist, then
     * reboot to apply. `rbampcfg phases <m> off` restores per-channel entries ch0 -> unit (others cleared). */
    /* addr <m> <hex> — re-point a fleet slot at another I2C address (persisted; REBOOT to apply), e.g. when a
     * module at 0x79 replaces one configured for 0x50 (otherwise only a hand-written /rbamp.json could do it). */
    /* count <n> — keep fleet slots 0..n-1, drop the rest (persisted; REBOOT to apply). A hub that saved the
     * 3-module default fleet but has one module otherwise logs "FAIL m1/m2 not found at boot" every boot and
     * runs a healer probe every 10 s. */
    if (strcmp(sub, "count") == 0) {
        if (argc < 3) { rbbench_reply("usage: rbampcfg count <n 1..%u>", (unsigned)cfg.module_count); return 1; }
        uint32_t n = (uint32_t)strtoul(argv[2], NULL, 10);
        if (n < 1 || n > cfg.module_count) { rbbench_reply("rbampcfg count: %lu out of 1..%u", (unsigned long)n, (unsigned)cfg.module_count); return 1; }
        uint8_t was = cfg.module_count;
        cfg.module_count = (uint8_t)n;
        esp_err_t se = rbgrid_storage_save_rbamp(&cfg);
        rbbench_reply("rbampcfg count: %u -> %lu modules, save %s (REBOOT to apply; units of dropped slots stay until DELETE /api/v1/units/<id>)",
                      (unsigned)was, (unsigned long)n, esp_err_to_name(se));
        return se == ESP_OK ? 0 : 1;
    }
    if (strcmp(sub, "addr") == 0) {
        if (argc < 4) { rbbench_reply("usage: rbampcfg addr <m> <hexaddr 08..77>"); return 1; }
        uint8_t m = (uint8_t)strtoul(argv[2], NULL, 10);
        uint32_t a = (uint32_t)strtoul(argv[3], NULL, 16);
        if (m >= cfg.module_count || m >= RBAMP_MAX_MODULES) { rbbench_reply("rbampcfg addr: bad module %u", m); return 1; }
        if (a < 0x08 || a > 0x77) { rbbench_reply("rbampcfg addr: 0x%02lX out of 0x08..0x77", (unsigned long)a); return 1; }
        uint8_t was = cfg.modules[m].addr;
        cfg.modules[m].addr = (uint8_t)a;
        esp_err_t se = rbgrid_storage_save_rbamp(&cfg);
        rbbench_reply("rbampcfg addr: module %u 0x%02X -> 0x%02X saved: %s — REBOOT to apply", m, was, (unsigned)a, esp_err_to_name(se));
        return se == ESP_OK ? 0 : 1;
    }
    if (strcmp(sub, "phases") == 0) {
        if (argc < 4) { rbbench_reply("usage: rbampcfg phases <m> <hexunit> <ct> | rbampcfg phases <m> off"); return 1; }
        uint8_t m = (uint8_t)strtoul(argv[2], NULL, 10);
        if (m >= cfg.module_count || m >= RBAMP_MAX_MODULES) { rbbench_reply("rbampcfg phases: bad module %u", m); return 1; }
        rbamp_module_cfg_t *mod = &cfg.modules[m];
        rbamp_entry_cfg_t keep[RBAMP_MAX_ENTRIES]; uint8_t nk = 0;
        for (uint8_t i = 0; i < mod->entry_count && i < RBAMP_MAX_ENTRIES; ++i)
            if (mod->entries[i].source == RBAMP_ENTRY_VOLTAGE) keep[nk++] = mod->entries[i];
        if (strcmp(argv[3], "off") != 0) {
            if (argc < 5) { rbbench_reply("usage: rbampcfg phases <m> <hexunit> <ct>"); return 1; }
            rbamp_entry_cfg_t g = { .source = RBAMP_ENTRY_PHASES,
                                    .ct_code = (uint8_t)strtoul(argv[4], NULL, 10),
                                    .unit_id = (uint16_t)strtoul(argv[3], NULL, 16), .ct_user = 0 };
            keep[nk++] = g;
        } else {
            bool grouped = false;
            for (uint8_t i = 0; i < mod->entry_count; ++i) if (mod->entries[i].source == RBAMP_ENTRY_PHASES) grouped = true;
            if (!grouped) {   /* already per-channel: rebuilding would drop the channel's unit (unit id → 0) */
                rbbench_reply("rbampcfg phases: module %u has no grouped entry — already per-channel, unchanged", m);
                return 0;
            }
            rbamp_entry_cfg_t c0 = { .source = 0, .ct_code = 0, .unit_id = 0, .ct_user = 0 };
            for (uint8_t i = 0; i < mod->entry_count; ++i)
                if (mod->entries[i].source == RBAMP_ENTRY_PHASES) { c0.ct_code = mod->entries[i].ct_code; c0.unit_id = mod->entries[i].unit_id; }
            keep[nk++] = c0;
        }
        memcpy(mod->entries, keep, sizeof(keep)); mod->entry_count = nk;
        esp_err_t se = rbgrid_storage_save_rbamp(&cfg);
        rbbench_reply("rbampcfg phases: module %u -> %u entries (grouped=%s) saved: %s — REBOOT to apply",
                      m, nk, strcmp(argv[3], "off") ? "yes" : "no", esp_err_to_name(se));
        return (se == ESP_OK) ? 0 : 1;
    }
    rbbench_reply("rbampcfg: bus sda=%d scl=%d poll=%ums inject=%d i_gain_m=%d v_gain_m=%d modules=%u",
                  cfg.sda_gpio, cfg.scl_gpio, (unsigned)cfg.poll_period_ms, (int)cfg.inject_enabled,
                  (int)(cfg.current_gain * 1000.0f), (int)(cfg.voltage_gain * 1000.0f),
                  (unsigned)cfg.module_count);
    for (uint8_t m = 0; m < cfg.module_count && m < RBAMP_MAX_MODULES; ++m) {
        const rbamp_module_cfg_t *mod = &cfg.modules[m];
        for (uint8_t en = 0; en < mod->entry_count && en < RBAMP_MAX_ENTRIES; ++en) {
            rbbench_reply("  mod%u@0x%02X entry%u: source=%u ct=%u unit=0x%04X",
                          (unsigned)m, (unsigned)mod->addr, (unsigned)en,
                          (unsigned)mod->entries[en].source, (unsigned)mod->entries[en].ct_code,
                          (unsigned)mod->entries[en].unit_id);
        }
    }
    return 0;
}

/* ===== External-flash STATISTICS test harness =========================
 * flashinfo                    — ext-flash status + gating (init/otp/fs/id/size/serial/available).
 * flashen <0|1> [enf 0|1]      — runtime toggle: "use external flash" + OTP-enforce (with/without
 *                                flash + anti-foreign-chip tests).
 * forceperiod <idx> <per>      — ACCELERATED long-period test: finalize a period NOW (store to
 *                                flash + roll up to the next level) without waiting real time.
 *                                per = stat-period: 1=15min 2=1h 3=day 4=week 5=month 6=year.
 * statread <hexid> <fper> <dt> — read records back from flash to confirm the write landed.
 *                                fper = STORAGE period: 0=HOUR 1=DAY 2=WEEK 3=MONTH 4=YEAR;
 *                                dt = 0=electrical 1=energy. Reports record count + first ts. */
/** `flashinfo`: external-flash status and gating. */
static int rbgrid_dbg_flashinfo(int argc, char **argv) {
    (void)argc; (void)argv;
    external_flash_state_t *st = external_flash_get_state();
    if (!st) { rbbench_reply("flashinfo: no state (ext-flash subsystem not initialized)"); return 1; }
    rbbench_reply("flash hw: init=%d otp_valid=%d fs_mounted=%d id=0x%08lx size=%luMB serial=0x%08lx",
                  (int)st->status.initialized, (int)st->status.otp_validated,
                  (int)st->status.filesystem_mounted, (unsigned long)st->status.flash_id,
                  (unsigned long)(st->status.flash_size / (1024 * 1024)),
                  (unsigned long)st->status.device_serial);
    rbbench_reply("flash gate: enabled=%d enforce_otp=%d => available=%d (last_err=%s)",
                  (int)rbpower_flash_get_enabled(), (int)rbpower_flash_get_enforce_otp(),
                  (int)rbpower_flash_is_available(),
                  st->status.error_message ? st->status.error_message : "none");
    return 0;
}
/** `flashen <0|1> [enforce_otp 0|1]`: toggle external-flash use and OTP enforcement at runtime. */
static int rbgrid_dbg_flashen(int argc, char **argv) {
    if (argc < 2) { rbbench_reply("usage: flashen <0|1> [enforce_otp 0|1]"); return 1; }
    uint32_t en = 1, enf = 1;
    if (!rbbench_parse_uint(argv[1], &en, 1)) { rbbench_reply("bad <0|1>"); return 1; }
    rbpower_flash_set_enabled(en != 0);
    if (argc >= 3 && rbbench_parse_uint(argv[2], &enf, 1)) rbpower_flash_set_enforce_otp(enf != 0);
    rbbench_reply("flashen: enabled=%d enforce_otp=%d => available=%d",
                  (int)rbpower_flash_get_enabled(), (int)rbpower_flash_get_enforce_otp(),
                  (int)rbpower_flash_is_available());
    return 0;
}
/** `forceperiod <idx> <per>`: finalize a period of a unit (by index) now. */
static int rbgrid_dbg_forceperiod(int argc, char **argv) {
    if (argc < 3) {
        rbbench_reply("usage: forceperiod <unit_index 0-%d> <period 1=15m 2=1h 3=day 4=week 5=mon 6=year>",
                      RBPOWER_MAX_HOME_UNITS - 1);
        return 1;
    }
    uint32_t idx = 0, per = 0;
    if (!rbbench_parse_uint(argv[1], &idx, RBPOWER_MAX_HOME_UNITS - 1)) { rbbench_reply("bad unit_index"); return 1; }
    if (!rbbench_parse_uint(argv[2], &per, RBPOWER_STAT_PERIOD_COUNT - 1) || per == 0) { rbbench_reply("bad period (1..6)"); return 1; }
    // FORCED finalize (test verb): same finalize path the timer uses, but with readiness ARMED so a
    // partial period actually STORES instead of the auto path's silent NOT-READY early-return (which
    // would report false success). aggregate accumulator -> store to flash -> rollup.
    if (!rbpower_finalize_period_callback_forced((rbpower_unit_id_t)idx, (rbpower_stat_period_type_t)per,
                                                 (uint64_t)esp_timer_get_time())) {
        rbbench_reply("forceperiod idx=%u per=%u: refused — no inputs, nothing to store", (unsigned)idx, (unsigned)per);
        return 1;
    }
    rbbench_reply("forceperiod idx=%u per=%u: forced finalize (readiness armed) — verify via statread/flashinfo "
                  "(check serial FLASH_WRITE log for store result)",
                  (unsigned)idx, (unsigned)per);
    return 0;
}

/** `statforce1h <hexid>`: force-finalize the accumulated 1HOUR period for a unit BY HEX ID and store a
 * HOUR flash-history record NOW (with the min/max envelope) instead of waiting for the wall-clock
 * hour boundary. For rbAmp units: run `period <module> 1` collect(s) first to feed the 1hour
 * accumulator (each collect rolls a 15-min result in), then call this. Cleaner than `forceperiod`:
 * takes the hex id (no unit-index guessing) and always runs the 1HOUR finalize (NOT the 15-min
 * accumulator path that forceperiod 1 would hit, which rbAmp units do not use). */
static int rbgrid_dbg_statforce1h(int argc, char **argv) {
    if (argc < 2) { rbbench_reply("usage: statforce1h <hex_unit_id>"); return 1; }
    uint32_t id = (uint32_t)strtoul(argv[1], NULL, 16);
    rbpower_err_t e = rbpower_force_finalize_period((rbpower_unit_id_t)id, RBPOWER_STAT_PERIOD_1HOUR);
    rbbench_reply("statforce1h 0x%04X (1HOUR): %s", (unsigned)id,
                  e == RBPOWER_OK ? "finalized (store+rollup) — verify via statcursor/statread"
                  : e == RBPOWER_ERR_INVALID_STATE ? "refused — no inputs, nothing to store (module absent / no collect yet)"
                  : "FAILED (unknown unit / not init)");
    return e == RBPOWER_OK ? 0 : 1;
}

/** `rmunit <hexid>`: DELETE a unit at runtime: stop its compute (gate off + drop from id-map +
 * disable accumulators), RECLAIM + ERASE its flash STATS slot, and persist units.json without it
 * (so it stays gone after restart). Tests the unit-deletion path end-to-end. */
static int rbgrid_dbg_rmunit(int argc, char **argv) {
    if (argc < 2) { rbbench_reply("usage: rmunit <hex_unit_id>"); return 1; }
    uint32_t id = (uint32_t)strtoul(argv[1], NULL, 16);
    rbpower_err_t e = rbpower_home_remove_unit((rbpower_unit_id_t)id);
    if (e != RBPOWER_OK) {
        rbbench_reply("rmunit 0x%04X: FAILED (%d — unknown/invalid unit)", (unsigned)id, (int)e);
        return 1;
    }
    esp_err_t se = rbgrid_storage_save_units(NULL);
    rbbench_reply("rmunit 0x%04X: REMOVED (compute stopped + slot erased/freed); units.json: %s",
                  (unsigned)id, esp_err_to_name(se));
    return 0;
}
/** `statread <hexid> <fper> <dt>`: read records back from flash (count + first timestamp). */
static int rbgrid_dbg_statread(int argc, char **argv) {
    if (argc < 4) { rbbench_reply("usage: statread <hex_unit_id> <flash_period 0=HOUR 1=DAY 2=WEEK 3=MON 4=YEAR> <dtype 0=elec 1=energy>"); return 1; }
    uint32_t id = (uint32_t)strtoul(argv[1], NULL, 16);
    uint32_t per = 0, dt = 0;
    rbbench_parse_uint(argv[2], &per, 8);
    rbbench_parse_uint(argv[3], &dt, 8);
    if (id == 0 || id > 0xFFFF) { rbbench_reply("bad hex_unit_id"); return 1; }
    const uint32_t MAXN = 32;
    uint8_t *buf = (uint8_t *)malloc(MAXN * 64);   // 64B/record headroom (electrical=28, energy=16)
    if (!buf) { rbbench_reply("statread: OOM"); return 1; }
    uint32_t got = 0;
    esp_err_t e = rbpower_flash_read_records((rbpower_unit_id_t)id, (rbpower_period_t)per,
                                             (rbpower_data_type_t)dt, 0, MAXN, buf, &got);
    if (e != ESP_OK) {
        rbbench_reply("statread 0x%04X per=%u dt=%u: %s (records=%u)",
                      (unsigned)id, (unsigned)per, (unsigned)dt, esp_err_to_name(e), (unsigned)got);
        free(buf); return 1;
    }
    uint32_t ts_first = got ? *(uint32_t *)buf : 0;   // every transfer record starts with a uint32 unix ts
    rbbench_reply("statread 0x%04X per=%u dt=%u: records=%u first_ts=%lu (persisted -> write path OK)",
                  (unsigned)id, (unsigned)per, (unsigned)dt, (unsigned)got, (unsigned long)ts_first);
    free(buf);
    return 0;
}
/** `statcursor <hex_unit_id> <flash_period> <dtype>`: explicit ring write-cursor (sector + index +
 * total sectors + newest_ts). Reconstructs via the same flash scan boot recovery uses. */
static int rbgrid_dbg_statcursor(int argc, char **argv) {
    if (argc < 4) { rbbench_reply("usage: statcursor <hex_unit_id> <flash_period 0=HOUR..4=YEAR> <dtype 0=elec 1=energy>"); return 1; }
    uint32_t id = (uint32_t)strtoul(argv[1], NULL, 16);
    uint32_t per = 0, dt = 0;
    rbbench_parse_uint(argv[2], &per, 8);
    rbbench_parse_uint(argv[3], &dt, 8);
    uint16_t sector = 0, idx = 0, total = 0; uint32_t newest = 0;
    esp_err_t e = rbpower_flash_get_cursor((rbpower_unit_id_t)id, (rbpower_period_t)per,
                                           (rbpower_data_type_t)dt, &sector, &idx, &total, &newest);
    if (e != ESP_OK) { rbbench_reply("statcursor 0x%04X per=%u dt=%u: %s", (unsigned)id, (unsigned)per, (unsigned)dt, esp_err_to_name(e)); return 1; }
    rbbench_reply("statcursor 0x%04X per=%u dt=%u: sector=%u/%u next_index=%u newest_ts=%lu",
                  (unsigned)id, (unsigned)per, (unsigned)dt, (unsigned)sector, (unsigned)total, (unsigned)idx, (unsigned long)newest);
    return 0;
}
/* stattest <hex_unit_id> [count=5] — FULL clear WRITE+READ test of the stats "disk":
 * writes N known electrical records (HOUR), reads them back, verifies field-exact (search by
 * timestamp), shows the ring cursor advance, and states the write/read time-separation + write
 * priority. Deterministic + human-readable PASS/CHECK. NOTE: writes synthetic records into the
 * unit's HOUR ring (pick a test unit). */
/** `flasherase`: erase all stats slots (DROPS HISTORY) then reboot to re-create buffers at the
 * current record format. Use once after a record-format change (e.g. the min/max expansion). */
static int rbgrid_dbg_flasherase(int argc, char **argv) {
    (void)argc; (void)argv;
    // Pre-reply BEFORE the erase: a full-region wipe takes tens of seconds (chunked, system stays
    // live) and the caller needs to see the verb was accepted, not 90 s of silence.
    rbbench_reply("flasherase: erasing the whole STATS region (chunked, ~30-90s) — wait for the result line");
    /* Marker FIRST, erase second: the erase is long and has crashed on an INCOMPATIBLE-state boot
     * — with the marker already gone, the NEXT boot takes the normal
     * "no marker → fresh region → auto-wipe + stamp current" path, so even a failed erase here
     * converges to a clean store. A wiped region must not keep a stale version marker anyway. */
    esp_err_t me = rbpower_flash_format_marker_clear();
    esp_err_t e = rbpower_flash_erase_all_slots();
    rbbench_reply("flasherase: %s (marker clear: %s) — REBOOT now (shutdown/reset) to re-create buffers at the new format",
                  esp_err_to_name(e), esp_err_to_name(me));
    return e == ESP_OK ? 0 : 1;
}
/* statfeed — SYNTHETIC 200ms feeder for a regular (generic-accumulated) unit. Drives the rbpower
 * 3sec→15MIN→HOUR→DAY+ chain with a deterministic constant load where no real source can keep up:
 * accel builds (RBGRID_STATS_TIME_SCALE>1 compresses boundaries below the rbAmp latch cadence) and
 * signed-accumulation tests (negative watts = export, no reversed CT needed). Mirrors the
 * rbamp_bridge inject_module() field set exactly. NOT for supply/vbus units (their specialized
 * accumulators early-return before the generic path — the feed would vanish). Test tool. */
#ifndef RBGRID_STATS_TIME_SCALE
/** @brief Statistics time acceleration factor (1 = real time; > 1 only in accelerated test builds). */
#define RBGRID_STATS_TIME_SCALE 1
#endif
static struct {
    volatile bool run;
    volatile bool quarters;   /* false = 200ms realtime feed; true = finished-QUARTER injector */
    volatile uint16_t unit;
    volatile float watts;
    TaskHandle_t th;
} s_statfeed;
/* QUARTER mode ('statfeed q ...', for high time scales): injects a FINISHED 15-min period result every
 * virtual quarter (real interval = 900s/scale) through the same path rbAmp/ESP-NOW results take —
 * no 200ms grid, so the chain runs correctly at scales where a quarter is shorter than the feed
 * grid (@2920: quarter = 308ms). Deterministic math: E_quarter = W×0.25 Wh (virtual), so
 * E_hour = W×1.0, E_day = W×24 — exact Σ-consistency checks down the chain. Injections are
 * free-running vs the virtual boundary clock, so on high scales an HOUR may catch 3–5 quarters —
 * judge by level-to-level sums, not per-record child counts. */
static volatile uint32_t s_statfeed_injected;   /* quarters injected since start (statfeed status) */
/** statfeed worker task: 200 ms realtime feed or boundary-synced quarter injector (see above). */
static void statfeed_task(void *arg) {
    (void)arg;
    /* QUARTER mode is BOUNDARY-SYNCED, not free-running: exactly ONE inject per VIRTUAL 15-min
     * bucket (uptime×scale/900), with catch-up if the task ever lags. A free-running
     * vTaskDelay(period) always sleeps ≥ the virtual quarter (tick quantization + loop work),
     * so hours would receive ~3.98 quarters on average and show a -1..-2%/day shortfall that looks
     * like pipeline loss.
     * Bucket-sync makes the injected count exact at ANY scale: E/hour == W, E/day == 24·W. */
    uint64_t last_vq = 0;
    bool seeded = false;
    while (s_statfeed.run && s_statfeed.quarters) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (!s_statfeed.run) break;
        uint64_t vq = (uint64_t)((esp_timer_get_time() * (uint64_t)RBGRID_STATS_TIME_SCALE / 1000000ULL) / 900ULL);
        if (!seeded) { last_vq = vq; seeded = true; continue; }
        while (vq > last_vq) {
            last_vq++;
            float p = s_statfeed.watts;
            float i = fabsf(p) / 230.0f;
            rbpower_home_inject_period_result((rbpower_unit_id_t)s_statfeed.unit,
                                              RBPOWER_STAT_PERIOD_15MIN,
                                              p, p, p * 0.25f, 900000u,
                                              230.0f, i, i, i);
            s_statfeed_injected++;
        }
    }
    while (s_statfeed.run) {
        rbpower_unit_realtime_average_200ms_t m;
        memset(&m, 0, sizeof(m));
        const float u = 230.0f;
        float p = s_statfeed.watts;
        float i = fabsf(p) / u;
        m.average_200ms.samples_count         = 1;
        m.is_valid                            = true;
        m.last_update_us                      = esp_timer_get_time();
        m.average_200ms.voltage_rms_avg       = u;
        m.average_200ms.current_rms_avg       = i;
        m.average_200ms.power_active_avg_w    = p;
        m.average_200ms.power_apparent_avg_va = u * i;
        m.average_200ms.power_factor_avg      = 1.0f;
        m.instant_power_net_w                 = p;
        rbpower_home_inject_200ms((rbpower_unit_id_t)s_statfeed.unit, &m);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    s_statfeed.th = NULL;
    vTaskDelete(NULL);
}
/** `statfeed [q] <hexid> <watts> | statfeed stop`: start/retarget/stop the synthetic feeder (see above). */
static int rbgrid_dbg_statfeed(int argc, char **argv) {
    if (argc < 2) {
        rbbench_reply("statfeed: %s unit=%04X P=%.1fW injected=%lu (E=%.1fWh)",
                      s_statfeed.th ? (s_statfeed.quarters ? "QUARTER running" : "200ms running") : "stopped",
                      (unsigned)s_statfeed.unit, (double)s_statfeed.watts,
                      (unsigned long)s_statfeed_injected,
                      (double)(s_statfeed_injected * s_statfeed.watts * 0.25f));
        return 0;
    }
    if (strcmp(argv[1], "stop") == 0) {
        s_statfeed.run = false;
        rbbench_reply("statfeed: stopping (injected=%lu quarters, E=%.1fWh)",
                      (unsigned long)s_statfeed_injected,
                      (double)(s_statfeed_injected * s_statfeed.watts * 0.25f));
        return 0;
    }
    bool quarters = (argc >= 2 && strcmp(argv[1], "q") == 0);
    int base = quarters ? 2 : 1;
    if (argc < base + 2) {
        rbbench_reply("usage: statfeed [q] <hex_unit_id> <watts> | statfeed stop  "
                      "(200ms feed @230V, or 'q' = finished-quarter injector for accel; "
                      "negative watts = export; regular units only)");
        return 1;
    }
    uint16_t id = (uint16_t)strtoul(argv[base], NULL, 16);
    float w = strtof(argv[base + 1], NULL);
    if (s_statfeed.th != NULL && quarters != s_statfeed.quarters) {
        rbbench_reply("statfeed: already running in %s mode — 'statfeed stop' first",
                      s_statfeed.quarters ? "quarter" : "200ms");
        return 1;
    }
    s_statfeed.quarters = quarters;
    s_statfeed.unit  = id;
    s_statfeed.watts = w;
    if (s_statfeed.th == NULL) {
        s_statfeed_injected = 0;
        s_statfeed.run = true;
        if (xTaskCreate(statfeed_task, "statfeed", 4096, NULL, 5, &s_statfeed.th) != pdPASS) {
            s_statfeed.run = false;
            rbbench_reply("statfeed: task create FAILED");
            return 1;
        }
    }
    if (quarters) {
        rbbench_reply("statfeed: QUARTER mode unit %04X P=%.1fW (inject every %lums real = 15min virtual; "
                      "E/quarter=%.2fWh, E/hour=%.1fWh expected)",
                      (unsigned)id, (double)w, (unsigned long)(900000UL / RBGRID_STATS_TIME_SCALE),
                      (double)(w * 0.25f), (double)w);
    } else {
        rbbench_reply("statfeed: unit %04X P=%.1fW i=%.3fA @230V (200ms feed; retarget by re-running, 'statfeed stop' to stop)",
                      (unsigned)id, (double)w, (double)(fabsf(w) / 230.0f));
    }
    return 0;
}
/** `statflow <hexid>`: flow-conservation counters of the quarter→HOUR path:
 * injected (inject_period_result 15MIN), aggregated into the hour acc, hours finalized, quarters
 * contained in stored hours. Conservation: injected == agg15; hour_q == agg15 − open-hour tail. */
static int rbgrid_dbg_statflow(int argc, char **argv) {
    if (argc < 2) { rbbench_reply("usage: statflow <hex_unit_id>"); return 1; }
    uint32_t id = (uint32_t)strtoul(argv[1], NULL, 16);
    uint32_t inj = 0, agg = 0, fin = 0, hq = 0;
    if (rbpower_statflow_get((rbpower_unit_id_t)id, &inj, &agg, &fin, &hq) != RBPOWER_OK) {
        rbbench_reply("statflow: unit %04X not found", (unsigned)id);
        return 1;
    }
    rbbench_reply("statflow %04X: injected=%lu agg15=%lu hour_fin=%lu hour_q=%lu (open-hour tail=%ld)",
                  (unsigned)id, (unsigned long)inj, (unsigned long)agg,
                  (unsigned long)fin, (unsigned long)hq, (long)((int32_t)agg - (int32_t)hq));
    return 0;
}
/** `timerstat`: period-scheduling diagnosis (for quarter-pipeline stalls): per-timer
 * state/armed/fires/next, stats-task liveness (evproc/last_act), drop/err/race/selfheal counters.
 * Distinguishes "one-shot timer dead" (act=0, selfheal grows) from "stats task blocked"
 * (act=1, fires grow, evproc frozen, last_act ages). */
static int rbgrid_dbg_timerstat(int argc, char **argv) {
    (void)argc; (void)argv;
    extern int rbpower_statistics_timers_diag(char* out, size_t cap);
    char *buf = (char *)malloc(512);   /* rbbench task stack is 4KB — keep buffers on the heap */
    if (!buf) { rbbench_reply("timerstat: oom"); return 1; }
    rbpower_statistics_timers_diag(buf, 512);
    rbbench_reply("%s", buf);
    free(buf);
    return 0;
}
/** `modscan`: rbAmp discovery self-heal (for modules=0 after boot): force an immediate
 * re-probe of configured-but-absent modules and print per-module state + healer counters.
 * Same heal the poll task runs every 10 s; the verb makes it on-demand + observable. */
static int rbgrid_dbg_modscan(int argc, char **argv) {
    (void)argc; (void)argv;
    char *buf = (char *)malloc(512);   /* rbbench task stack is 4KB — keep buffers on the heap */
    if (!buf) { rbbench_reply("modscan: oom"); return 1; }
    rbamp_bridge_rescan(buf, 512);
    rbbench_reply("%s", buf);
    free(buf);
    return 0;
}
/* etstat [hex_unit_id] — et_in/et_out totalizers + feed diagnostics.
 * Works on every build: reads the RAM totalizers, not v4 records. */
/** Shared body of `stdreg` / `basreg`: raw READ of a module register block (16-bit BE pointer for STD, 8-bit for
 * BASIC, auto-increment), n ≤ 32 bytes. Read-only; bus lock held. For module diagnostics the library has no
 * getter for (e.g. REG_I2C_DIAG 0x013A = u16 own-address matches · BERR · OVR). */
static int raw_reg_read(int argc, char **argv, bool ptr16, const char *verb) {
    uint32_t addr = 0, reg = 0, n = 6;
    if (argc < 3 || !rbbench_parse_uint(argv[1], &addr, 0x77) || !rbbench_parse_uint(argv[2], &reg, ptr16 ? 0xFFFF : 0xFF) ||
        (argc >= 4 && (!rbbench_parse_uint(argv[3], &n, 32) || n == 0))) {
        rbbench_reply(ptr16 ? "usage: stdreg <0xaddr> <0xreg> [n<=32]   e.g. stdreg 0x50 0x013A 6"
                            : "usage: basreg <0xaddr> <0xreg> [n<=32]   e.g. basreg 0x50 0x4B 2"); return 1;
    }
    i2c_master_bus_handle_t bus = rbamp_bridge_get_bus();
    if (!bus) { rbbench_reply("%s: no bus", verb); return 1; }
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = (uint16_t)addr, .scl_speed_hz = 50000 };
    i2c_master_dev_handle_t dev;
    uint8_t ptr[2] = { (uint8_t)(reg >> 8), (uint8_t)reg }, buf[32];
    rbamp_bridge_bus_lock();
    esp_err_t e = i2c_master_bus_add_device(bus, &dc, &dev);
    if (e == ESP_OK) {
        e = ptr16 ? i2c_master_transmit_receive(dev, ptr, 2, buf, n, 100) : i2c_master_transmit_receive(dev, &ptr[1], 1, buf, n, 100);
        i2c_master_bus_rm_device(dev);
    }
    rbamp_bridge_bus_unlock();
    if (e != ESP_OK) { rbbench_reply("%s 0x%02lX 0x%04lX: %s", verb, (unsigned long)addr, (unsigned long)reg, esp_err_to_name(e)); return 1; }
    char out[120]; int p = 0;
    for (uint32_t i = 0; i < n && p < (int)sizeof(out) - 4; i++) p = rb_scat(out, sizeof(out), p, "%02X ", buf[i]);
    rbbench_reply("%s 0x%02lX 0x%04lX [%lu]: %s", verb, (unsigned long)addr, (unsigned long)reg, (unsigned long)n, out);
    return 0;
}
/** `stdreg <hexaddr> <hexreg> [n]`: raw read of an STD-line module register block. */
static int rbgrid_dbg_stdreg(int argc, char **argv) { return raw_reg_read(argc, argv, true, "stdreg"); }
/** `basreg <hexaddr> <hexreg> [n]`: the same for a BASIC-line module (8-bit pointer), e.g. I2C_ERR_COUNT 0x4B (u16) */
static int rbgrid_dbg_basreg(int argc, char **argv) { return raw_reg_read(argc, argv, false, "basreg"); }
/** `digest <m> [n]`: BASIC DIGEST per window (rbamp-esp-idf 1.7.2-rc6): n windows, seq step, signed P vs read_all, read time */
static int rbgrid_dbg_digest(int argc, char **argv) {
    uint32_t m = 0, n = 10;
    if (argc < 2 || !rbbench_parse_uint(argv[1], &m, RBAMP_MAX_MODULES - 1) || (argc >= 3 && !rbbench_parse_uint(argv[2], &n, 50))) {
        rbbench_reply("usage: digest <module> [windows<=50]"); return 1;
    }
    static char out[1200];
    rbamp_bridge_digest((uint8_t)m, (int)n, out, sizeof(out));
    for (const char *a = out; *a; ) {
        const char *b = strstr(a, " | ");
        int len = b ? (int)(b - a) : (int)strlen(a);
        rbbench_reply("%.*s", len, a);
        a = b ? b + 3 : a + len;
    }
    return 0;
}
/** `libcheck <m>`: rbamp-esp-idf 1.7.0 STD call check (library validation); restores the start current. */
static int rbgrid_dbg_libcheck(int argc, char **argv) {
    uint32_t m = 0;
    if (argc < 2 || !rbbench_parse_uint(argv[1], &m, RBAMP_MAX_MODULES - 1)) { rbbench_reply("usage: libcheck <module>"); return 1; }
    static char out[640];
    rbamp_bridge_libcheck((uint8_t)m, out, sizeof(out));
    for (const char *a = out; *a; ) {     /* console lines are 160 B: one reply per section */
        const char *b = strstr(a, " | ");
        int len = b ? (int)(b - a) : (int)strlen(a);
        rbbench_reply("libcheck m%lu: %.*s", (unsigned long)m, len, a);
        a = b ? b + 3 : a + len;
    }
    return 0;
}
/** `appw`: this hub's AP SSID + password (USB console; remotely only with an admin token — TOKEN_ONLY) */
static int rbgrid_dbg_appw(int argc, char **argv) {
    (void)argc; (void)argv;
    wifi_config_t c;
    if (esp_wifi_get_config(WIFI_IF_AP, &c) != ESP_OK) { rbbench_reply("appw: AP not configured"); return 1; }
    char ssid[33]; memcpy(ssid, c.ap.ssid, 32); ssid[32] = '\0';
    char pw[65];   memcpy(pw, c.ap.password, 64); pw[64] = '\0';
    rbbench_reply("appw ssid=%s password=%s", ssid, pw[0] ? pw : "(open)");
    return 0;
}
/** `drdy [gpio|off]`: DRDY (data-ready) line of the rbAmp modules: status, or set + persist + apply */
static int rbgrid_dbg_drdy(int argc, char **argv) {
    char out[320];
    bool set = argc >= 2;
    int g = -1;
    if (set && strcmp(argv[1], "off") != 0) g = atoi(argv[1]);
    rbamp_bridge_drdy(g, set, out, sizeof(out));
    for (const char *a = out; *a; ) {     /* console replies are 160 B: one per section */
        const char *b = strstr(a, " | ");
        int len = b ? (int)(b - a) : (int)strlen(a);
        rbbench_reply("%.*s", len, a);
        a = b ? b + 3 : a + len;
    }
    return 0;
}
/** `stacks`: minimum free stack (bytes) ever seen per task. Names of every task the firmware / IDF starts;
 * absent ones are skipped. */
static int rbgrid_dbg_stacks(int argc, char **argv) {
    (void)argc; (void)argv;
    static const char *const NAMES[] = {
        "rbamp_poll", "rbpwr_stats", "esp_timer", "tiT", "sys_evt", "httpd", "mqtt_task", "modota", "stats_pflush",
        "ha_disc", "espnow_beacon", "espnow_inject", "espnow_autostart", "rbtime_sync", "ws_maintenance",
        "ota_confirm", "base_w", "rbevlog", "rbbench", "flash_write_task", "wifi", "ipc0", "ipc1", "IDLE0", "IDLE1",
    };
    char line[150]; int p = 0;
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        char q[configMAX_TASK_NAME_LEN];   /* FreeRTOS keeps 15 chars and ASSERTS on a longer query (a 16-char name
                                             * would panic the hub) */
        strlcpy(q, NAMES[i], sizeof(q));
        TaskHandle_t h = xTaskGetHandle(q);
        if (!h) continue;
        UBaseType_t hw = uxTaskGetStackHighWaterMark(h);   /* bytes on ESP-IDF (StackType_t = uint8_t) */
        int n = snprintf(line + p, sizeof(line) - p, "%s%s=%u", p ? " " : "", NAMES[i], (unsigned)hw);
        if (n < 0 || p + n >= (int)sizeof(line) - 1) {   /* flush the line (console replies are 160 B) */
            line[p] = '\0';
            rbbench_reply("stacks %s", line);
            p = snprintf(line, sizeof(line), "%s=%u", NAMES[i], (unsigned)hw);
        } else {
            p += n;
        }
    }
    if (p) rbbench_reply("stacks %s", line);
    rbbench_reply("stacks: min free bytes since boot; < 512 is tight");
    return 0;
}
/** `savecal <m>`: SAVE_CAL on module m (rbamp_save_gains): persists the module's CURRENT calibration and, on STD
 * 0x7A+, the cal_ok marker. Maintenance operation; allowed over /api/v1/debug/cmd with the write token
 * because OTA-only hubs have no console. */
static int rbgrid_dbg_savecal(int argc, char **argv) {
    uint32_t m = 0;
    if (argc < 2 || !rbbench_parse_uint(argv[1], &m, 15)) { rbbench_reply("usage: savecal <module>"); return 1; }
    esp_err_t e = rbamp_bridge_save_gains((uint8_t)m);
    rbbench_reply("savecal m%lu: %s", (unsigned long)m, esp_err_to_name(e));
    return e == ESP_OK ? 0 : 1;
}
/** `crashtest`: deliberate LoadProhibited to validate panic forensics (pc/bt in the next boot line). USB console only. */
static int rbgrid_dbg_crashtest(int argc, char **argv) {
    (void)argc; (void)argv;
    rbbench_reply("crashtest: dereferencing NULL now");
    vTaskDelay(pdMS_TO_TICKS(100));
    volatile uint32_t *p = (volatile uint32_t *)0x00000004;
    return (int)*p;
}
/* ---- Diagnostics: `diag [on|off|<period_s>]` + the "#DIAG " sink ---- */
/** rbpower diag sink: forward each diag line to the console prefixed with "#DIAG ". */
static void rbgrid_diag_sink(const char *line) { rbbench_reply("#DIAG %s", line); }
/** `diag [on|off|<period_s>]`: switch the console diag trace and its reconciliation period. */
static int rbgrid_dbg_diag(int argc, char **argv) {
    if (argc >= 2) {
        if (strcasecmp(argv[1], "on") == 0)       rbpower_diag_set(true,  rbpower_diag_period_cfg_s());
        else if (strcasecmp(argv[1], "off") == 0) rbpower_diag_set(false, rbpower_diag_period_cfg_s());
        else {
            uint32_t p = 0;
            if (!rbbench_parse_uint(argv[1], &p, 86400)) { rbbench_reply("usage: diag [on|off|<period_s 0..86400>]"); return 1; }
            rbpower_diag_set(true, p);
        }
    }
    rbbench_reply("diag %s period=%lus — lines: '#DIAG m<N> +<s> cnt=.. int=.. dev=..%%' per period, 'QTR'/'HOUR'/'hour persist'/'gap' events, 'FAIL ' = invariant broken",
                  rbpower_diag_console_on() ? "ON" : "off", (unsigned long)rbpower_diag_period_cfg_s());
    return 0;
}
/** `clock <unix_ts | HH:MM[:SS]>`: set the hub wall clock directly (testing: drive period boundaries in
 * minutes instead of waiting for them). HH:MM = today's LOCAL date. SNTP re-syncs later; use a local
 * NTP with an offset for reboot scenarios. */
static int rbgrid_dbg_clock(int argc, char **argv) {
    if (argc < 2) { rbbench_reply("usage: clock <unix_ts | HH:MM[:SS]>  (local time today)"); return 1; }
    uint64_t ts = 0;
    if (strchr(argv[1], ':')) {
        int hh = 0, mm = 0, ss = 0;
        if (sscanf(argv[1], "%d:%d:%d", &hh, &mm, &ss) < 2 || hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 59) {
            rbbench_reply("clock: bad time '%s'", argv[1]); return 1;
        }
        time_t now = time(NULL); struct tm lt; localtime_r(&now, &lt);
        lt.tm_hour = hh; lt.tm_min = mm; lt.tm_sec = ss; lt.tm_isdst = -1;
        time_t t = mktime(&lt);
        if (t <= 0) { rbbench_reply("clock: mktime failed"); return 1; }
        ts = (uint64_t)t;
    } else {
        ts = (uint64_t)strtoull(argv[1], NULL, 10);
        if (ts < 1600000000ULL) { rbbench_reply("clock: unix_ts must be > 1600000000"); return 1; }
    }
    rbpower_err_t e = rbtime_set_manual_time(ts);
    time_t t2 = (time_t)ts; struct tm lt2; localtime_r(&t2, &lt2);
    rbbench_reply("clock set ts=%llu local=%04d-%02d-%02d %02d:%02d:%02d: %s", (unsigned long long)ts,
                  lt2.tm_year + 1900, lt2.tm_mon + 1, lt2.tm_mday, lt2.tm_hour, lt2.tm_min, lt2.tm_sec,
                  e == RBPOWER_OK ? "OK" : "ERROR");
    return e == RBPOWER_OK ? 0 : 1;
}
/** `vmod`: virtual STD module in a fleet slot (software counters; see rbamp_bridge.c). */
static int rbgrid_dbg_vmod(int argc, char **argv) {
    char line[200];
    rbamp_bridge_virt_cmd(argc, argv, line, sizeof(line));
    rbbench_reply("%s", line);
    return 0;
}
/** `pollpause <m> <sec>`: stop polling module m for <sec> s (software poll hole; 0 = resume). */
static int rbgrid_dbg_pollpause(int argc, char **argv) {
    if (argc < 3) { rbbench_reply("usage: pollpause <module> <seconds>  (skip polling = software poll hole; 0 = resume)"); return 1; }
    uint32_t m = (uint32_t)strtoul(argv[1], NULL, 10), sec = (uint32_t)strtoul(argv[2], NULL, 10);
    if (rbamp_bridge_poll_pause((uint8_t)m, sec) != 0) { rbbench_reply("pollpause: bad module %lu", (unsigned long)m); return 1; }
    rbbench_reply("pollpause m%lu %lus%s", (unsigned long)m, (unsigned long)sec, sec ? " (expect '#DIAG m poll hole' at resume)" : " (resumed)");
    return 0;
}
/* leakprobe <n> — call the history building blocks n times IN-PROCESS (no HTTP) and print the heap
 * delta of each stage, to localize a per-request leak on /api/v1/history. */
extern "C" char* rbgrid_history_get_data(rbpower_unit_id_t, rbpower_period_t, rbpower_data_type_t, uint32_t, uint32_t);
extern "C" esp_err_t rbpower_flash_get_record_count(rbpower_unit_id_t, rbpower_period_t, rbpower_data_type_t, uint32_t*);
extern "C" rbgrid_json_context_t *rbwebserver_core_json_ctx(void);
/** `leakprobe <n>`: heap delta of the history building blocks (see above). */
static int rbgrid_dbg_leakprobe(int argc, char **argv) {
    uint32_t n = (argc >= 2) ? (uint32_t)strtoul(argv[1], NULL, 10) : 100;
    size_t a, b;
    a = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    for (uint32_t i = 0; i < n; i++) { uint32_t c = 0; rbpower_flash_get_record_count(0x0006, RBPOWER_PERIOD_DAY, RBPOWER_DATA_TYPE_ENERGY, &c); }
    b = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    rbbench_reply("leakprobe record_count x%lu: %+ld B", (unsigned long)n, (long)b - (long)a);
    a = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    for (uint32_t i = 0; i < n; i++) { char *j = rbgrid_history_get_data(0x0006, RBPOWER_PERIOD_DAY, RBPOWER_DATA_TYPE_ENERGY, 0, 1); free(j); }
    b = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    rbbench_reply("leakprobe history_get_data x%lu: %+ld B", (unsigned long)n, (long)b - (long)a);
    a = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    for (uint32_t i = 0; i < n; i++) { char *j = rbgrid_json_create_sensors_list(rbwebserver_core_json_ctx()); free(j); }
    b = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    rbbench_reply("leakprobe sensors_list x%lu: %+ld B", (unsigned long)n, (long)b - (long)a);
    return 0;
}
/** `etstat [hexid]`: et_in/et_out totalizers, feed diagnostics, reboot-gap state and NVS usage. */
static int rbgrid_dbg_etstat(int argc, char **argv) {
    uint16_t want_id = 0; bool filter = false;
    if (argc >= 2) { want_id = (uint16_t)strtoul(argv[1], NULL, 16); filter = true; }
    char *buf = (char *)malloc(768);
    if (!buf) { rbbench_reply("etstat: oom"); return 1; }
    int n = snprintf(buf, 768, "src=%s",
                     (rbpower_lifetime_restore_src() == 1) ? "rtc"
                     : (rbpower_lifetime_restore_src() == 2) ? "nvs" : "seed");
    uint16_t ids[RBPOWER_MAX_HOME_UNITS];
    rbpower_home_lock();
    uint8_t cnt = rbpower_home_list_unit_ids(ids, RBPOWER_MAX_HOME_UNITS);
    for (uint8_t k = 0; k < cnt && n < 700; k++) {
        uint16_t id = ids[k];
        if (filter && id != want_id) continue;
        int64_t et_in = 0, et_out = 0, seed_in = 0, seed_out = 0;
        uint32_t feeds = 0; time_t last_ts = 0;
        rbpower_lifetime_get_totalizers(id, &et_in, &et_out);
        rbpower_home_get_et_diag(find_unit_index_by_id(id), &feeds, &last_ts, &seed_in, &seed_out);
        n = rb_scat(buf, 768, n,
                      " | %04X et_in_mWh=%lld et_out_mWh=%lld feeds=%lu last_feed_ts=%lld boot_seed=%lld/%lld",
                      id, (long long)et_in, (long long)et_out, (unsigned long)feeds,
                      (long long)last_ts, (long long)seed_in, (long long)seed_out);
    }
    rbpower_home_unlock();
#if RBGRID_RBAMP_FLEET
    for (uint8_t m = 0; m < RBAMP_MAX_MODULES && n < 740; ++m) {
        char g[96];
        if (rbamp_bridge_gap_status(m, g, sizeof(g)) > 0 && !strstr(g, "=none")) n = rb_scat(buf, 768, n, " | %s", g);
    }
#endif
    {   /* NVS budget (anchors / open hour / net-metering all live here; a full partition drops them silently) */
        nvs_stats_t st;
        if (nvs_get_stats(NULL, &st) == ESP_OK && n < 740)
            n = rb_scat(buf, 768, n, " | nvs used=%u free=%u ns=%u", (unsigned)st.used_entries, (unsigned)st.free_entries, (unsigned)st.namespace_count);
    }
    rbbench_reply("%s", buf);
    free(buf);
    return 0;
}
/** `gridfeed <ph> <v> [f]`: test fixture — publish one grid phase voltage as a module would (no hardware). */
static int rbgrid_dbg_gridfeed(int argc, char **argv) {
    if (argc < 3) { rbbench_reply("usage: gridfeed <ph 1..3> <v> [f]"); return 1; }
    uint8_t ph = (uint8_t)strtoul(argv[1], NULL, 10);
    float v = (float)atof(argv[2]);
    float f = (argc >= 4) ? (float)atof(argv[3]) : NAN;
    rbpower_err_t r = rbpower_home_set_grid_phase(ph, v, f, 0, (uint8_t)(ph ? ph - 1 : 0));
    if (r == RBPOWER_OK && !isnan(f)) rbpower_home_set_system_frequency(f);
    rbbench_reply("gridfeed L%u v=%.2f f=%.2f rc=%d", ph, (double)v, (double)f, (int)r);
    return r == RBPOWER_OK ? 0 : 1;
}
/** `gridstat`: grid (mains) voltage per phase as the modules measure it (rbpower_home_get_grid). */
static int rbgrid_dbg_gridstat(int argc, char **argv) {
    (void)argc; (void)argv;
    rbpower_grid_phase_t g[3];
    uint8_t n = rbpower_home_get_grid(g);
    char line[200]; int k = snprintf(line, sizeof(line), "grid n=%u mean=%.2fV", (unsigned)n, (double)rbpower_home_grid_voltage(0));
    for (uint8_t i = 0; i < n && k < (int)sizeof(line) - 1; ++i)
        k = rb_scat(line, sizeof(line), k, " | L%u=%.2fV f=%.2f m%u/v%u age=%llds", g[i].ph, (double)g[i].v, (double)g[i].f,
                      g[i].src_module, g[i].src_vch, (long long)((esp_timer_get_time() - g[i].ts_us) / 1000000));
    rbbench_reply("%s", line);
    return 0;
}
/* stattest record type follows the ON-FLASH format (raw ring test): v4 struct post-flip,
 * v3 transfer struct before. Payload field names are identical; only the ts location differs. */
#if RBPOWER_RECORD_FORMAT_VERSION >= 4
typedef rbstats_electrical_v4_t stattest_rec_t;   ///< record type written by `stattest`
#define STATTEST_TS(r) ((r).h.ts)                 ///< timestamp field of a stattest record
#else
typedef rbpower_electrical_transfer_t stattest_rec_t;
#define STATTEST_TS(r) ((r).timestamp)
#endif
/** `stattest <hexid> [count]`: write/read/verify test of the stats flash ring (see the note above flasherase). */
static int rbgrid_dbg_stattest(int argc, char **argv) {
    if (argc < 2) { rbbench_reply("usage: stattest <hex_unit_id> [count=5]"); return 1; }
    uint32_t id = (uint32_t)strtoul(argv[1], NULL, 16);
    uint32_t n = 5;
    if (argc >= 3) rbbench_parse_uint(argv[2], &n, 10);
    if (id == 0 || id > 0xFFFF) { rbbench_reply("bad hex_unit_id"); return 1; }
    if (n < 1) { n = 1; }
    if (n > 10) { n = 10; }
    const rbpower_period_t per = RBPOWER_PERIOD_HOUR;
    const rbpower_data_type_t dt = RBPOWER_DATA_TYPE_ELECTRICAL;

    rbbench_reply("=== STATTEST 0x%04X HOUR electrical n=%u ===", (unsigned)id, (unsigned)n);
    rbbench_reply("[0] flash available=%d writers_waiting=%d", (int)rbpower_flash_is_available(), rbpower_flash_writers_waiting());
    if (!rbpower_flash_is_available()) { rbbench_reply("RESULT: SKIP (flash unavailable — see flashinfo/flashen)"); return 1; }

    uint16_t s0 = 0, i0 = 0, t0 = 0; uint32_t nt0 = 0;
    rbpower_flash_get_cursor(id, per, dt, &s0, &i0, &t0, &nt0);
    rbbench_reply("[1] cursor before: sector=%u/%u next_index=%u newest_ts=%lu", (unsigned)s0, (unsigned)t0, (unsigned)i0, (unsigned long)nt0);

    // [2] WRITE n known records (deterministic fixed-point values).
    uint32_t base = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    stattest_rec_t wr[10];
    int wrote = 0;
    for (uint32_t k = 0; k < n; k++) {
        memset(&wr[k], 0, sizeof(wr[k]));
        STATTEST_TS(wr[k])     = base + k;
        wr[k].voltage_rms_v10  = (uint16_t)(2300 + k);   // 230.0V + k*0.1
        wr[k].current_rms_ma   = (uint16_t)(1000 + k);   // 1.000A + k mA
        wr[k].power_active_w10 = (int16_t)(1000 + k);    // 100.0W + k*0.1
        wr[k].frequency_hz10   = 50;
        // envelope min/max (new format)
        wr[k].voltage_min_v10  = (uint16_t)(2295 + k);   // 229.5V
        wr[k].voltage_max_v10  = (uint16_t)(2305 + k);   // 230.5V
        wr[k].current_min_ma   = (uint16_t)(900 + k);
        wr[k].current_max_ma   = (uint16_t)(1100 + k);
        wr[k].power_min_w10    = (int16_t)(950 + k);     // 95.0W
        wr[k].power_max_w10    = (int16_t)(1050 + k);    // 105.0W
        esp_err_t we = rbpower_flash_write_record(id, per, dt, &wr[k], sizeof(wr[k]));
        {   /* companion ENERGY record (same ts) — the migration repro needs the energy ring populated too */
            rbpower_energy_transfer_t en = {0};
            en.timestamp = STATTEST_TS(wr[k]); en.energy_consumed_wh = 25 + k; en.energy_net_wh = (int32_t)(25 + k);
            (void)rbpower_flash_write_record(id, per, RBPOWER_DATA_TYPE_ENERGY, &en, sizeof(en));
        }
        rbbench_reply("[2] write k=%u ts=%lu V=%.1f P=%.1fW Pmin=%.1f Pmax=%.1f -> %s",
                      (unsigned)k, (unsigned long)STATTEST_TS(wr[k]), wr[k].voltage_rms_v10 / 10.0,
                      wr[k].power_active_w10 / 10.0, wr[k].power_min_w10 / 10.0,
                      wr[k].power_max_w10 / 10.0, esp_err_to_name(we));
        if (we == ESP_OK) wrote++;
    }

    // [3] READ a newest window ONCE, then verify each written record by timestamp (field-exact).
    // Window on the HEAP, not the stack — 64×32B = 2KB would overflow the rbbench task stack
    // (4KB) together with the flash call chain (non-deterministic SW_CPU_RESET).
    const uint32_t window = 64;
    stattest_rec_t *rd = (stattest_rec_t*)malloc(window * sizeof(stattest_rec_t));
    if (!rd) { rbbench_reply("RESULT: SKIP (OOM read window)"); return 1; }
    uint32_t rdn = 0;
    for (uint32_t r = 0; r < window; r++) {
        if (rbpower_flash_read_record(id, per, dt, r, &rd[rdn], sizeof(rd[0])) != ESP_OK) break;
        rdn++;
    }
    int verified = 0;
    for (uint32_t k = 0; k < n; k++) {
        bool found = false, match = false;
        for (uint32_t r = 0; r < rdn; r++) {
            if (STATTEST_TS(rd[r]) == STATTEST_TS(wr[k])) {
                found = true;
                match = (memcmp(&rd[r], &wr[k], sizeof(wr[k])) == 0);
                break;
            }
        }
        if (match) verified++;
        rbbench_reply("[3] verify ts=%lu: %s", (unsigned long)STATTEST_TS(wr[k]),
                      match ? "MATCH" : (found ? "FIELD-MISMATCH" : "NOT-FOUND"));
    }
    rbbench_reply("[3] read window scanned %u records", (unsigned)rdn);

    uint16_t s1 = 0, i1 = 0, t1 = 0; uint32_t nt1 = 0;
    rbpower_flash_get_cursor(id, per, dt, &s1, &i1, &t1, &nt1);
    rbbench_reply("[4] cursor after: sector=%u/%u next_index=%u newest_ts=%lu (was idx=%u ts=%lu)",
                  (unsigned)s1, (unsigned)t1, (unsigned)i1, (unsigned long)nt1, (unsigned)i0, (unsigned long)nt0);
    rbbench_reply("[5] write/read: TIME-SEPARATED via flash mutex; WRITE PRIORITY (readers defer while writers waiting)");
    rbbench_reply("RESULT: %s  (wrote %d/%u, verified %d/%u, newest_ts %lu->%lu)",
                  (verified == (int)n && wrote == (int)n) ? "PASS" : "CHECK",
                  wrote, (unsigned)n, verified, (unsigned)n, (unsigned long)nt0, (unsigned long)nt1);
    free(rd);
    return 0;
}

#endif  /* RBGRID_RBBENCH — debug console verbs */

/** Period-boundary callback from the stats task: at each 15-min boundary, request the rbAmp period collect. */
static void rbgrid_rbamp_period_cb(rbpower_stat_period_type_t period, uint64_t ts) {
    (void)ts;
    if (period != RBPOWER_STAT_PERIOD_15MIN) return;
    /* hand the collect to the bridge poll task: done here (rbpwr_stats, task watchdog 5 s) a slow BASIC bus
     * transaction at the quarter boundary could starve the stats task and reset the hub */
    rbamp_bridge_request_collect(RBPOWER_STAT_PERIOD_15MIN, rbpower_stats_quarter_boundary_now());
}
#if RBGRID_RBBENCH
}
#endif

#if RBGRID_RBAMP_FLEET
/** Is this rbAmp (module_index, channel) still bound to a unit in the PERSISTED
 * fleet (/rbamp.json)? Returns false ONLY when a matching entry exists with unit_id==0 — i.e. a unit
 * was deleted via the API and rbamp_bridge_unbind_unit cleared its fleet binding. No saved fleet, an
 * absent/unknown entry, or a module index past the saved set → true (default-bound), so a fresh
 * device (no /rbamp.json yet) still creates all the default units. */
static bool rbamp_channel_bound(const rbamp_bridge_config_t *fleet, uint8_t module_index, uint8_t channel) {
    if (!fleet) return true;
    /* A slot BEYOND the saved fleet is not configured at all (dropped by `rbampcfg count`) — no default
     * units for it (otherwise units of dropped slots would come back at every boot after deletion). */
    if (module_index >= fleet->module_count || module_index >= RBAMP_MAX_MODULES) return false;
    const rbamp_module_cfg_t *mod = &fleet->modules[module_index];
    for (uint8_t e = 0; e < mod->entry_count && e < RBAMP_MAX_ENTRIES; ++e) {
        if (mod->entries[e].source == channel) return mod->entries[e].unit_id != 0;
    }
    return true;
}
#endif

// ===== TEMPORARY SOLUTION: Forward declaration for access to internal structures =====
// This is temporary until the API is fully developed
/** @brief Mirror of the private rbgrid config-context layout (rbgrid_config_api), used to reach the built
 *         configuration. Must match the component's definition field for field. */
struct rbgrid_config_context_s {
    rbpower_extended_config_t* extended_config;    ///< extended configuration
    rbpower_hardware_config_t* hardware_config;    ///< hardware configuration
    rbpower_electrical_network_t* network;         ///< electrical network being built
    rbpower_unit_definition_t* units_array;        ///< unit definitions
    uint16_t units_allocated;                      ///< capacity of units_array
    uint16_t units_count;                          ///< used entries of units_array
    struct {
        int next_sensor_id;
        int next_unit_id;
        uint8_t adc_channels_used;
        bool has_voltage_bus;
        bool has_main_supply;
        bool has_alternative_supply;
    } builder;                                     ///< builder state (layout mirror only)
    struct {
        bool base_initialized;
        bool network_configured;
        bool sensors_configured;
        bool units_configured;
    } status;                                      ///< build-step flags
    bool is_initialized;                           ///< context initialized
    bool is_applied;                               ///< configuration applied
};
typedef struct rbgrid_config_context_s rbgrid_config_context_t;   ///< see rbgrid_config_context_s

// (feature flags RBAMP_BRIDGE_ENABLE / RBGRID_RBAMP_FLEET / RBGRID_BENCH_FULL / RBGRID_RBBENCH /
//  RBGRID_WIFI are defined at the top of the file — needed by the early includes.)

// 🔴 SAFETY: the test-rig load bank (PCF8574 @0x20/0x25 on I2C1 19/23) drives relays. At power-on
// the PCF8574 outputs float HIGH → relays default ON → live load on the AC line before any
// software control. rbbench does a fail-safe OFF in its init, but the wifi/telemetry combined
// build has no rbbench. Drive both banks to 0x00 (relays OFF) as the FIRST thing at boot, before
// anything else can matter. PCF8574 latches the written value, so it stays OFF after we close the
// bus. Gated to the no-rbbench build; the debug-bridge build's rbbench already fail-safes the bank.
#if RBGRID_RBAMP_FLEET && !defined(CONFIG_RBGRID_DEBUG_BRIDGE)
#define RBGRID_BENCH_RELAY_FAILSAFE 1   ///< 1 = drive the test-rig load-bank relays OFF at boot
/** Drive both PCF8574 load-bank expanders to 0x00 (relays OFF) at boot (see the SAFETY note above). */
static void bench_relays_failsafe_off(void) {
    i2c_master_bus_config_t bc = {};
    bc.i2c_port = 1; bc.sda_io_num = (gpio_num_t)19; bc.scl_io_num = (gpio_num_t)23;
    bc.clk_source = I2C_CLK_SRC_DEFAULT; bc.glitch_ignore_cnt = 7;
    bc.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) {
        ESP_LOGW("SAFETY", "load-bank bus init failed — relays NOT fail-safed!");
        return;
    }
    const uint8_t addrs[2] = { 0x20, 0x25 };
    const uint8_t off = 0x00;
    for (int i = 0; i < 2; i++) {
        i2c_device_config_t dc = {};
        dc.dev_addr_length = I2C_ADDR_BIT_LEN_7; dc.device_address = addrs[i]; dc.scl_speed_hz = 50000;
        i2c_master_dev_handle_t dev = NULL;
        if (i2c_master_bus_add_device(bus, &dc, &dev) == ESP_OK) {
            esp_err_t w = i2c_master_transmit(dev, &off, 1, 100);
            ESP_LOGI("SAFETY", "load-bank relays OFF 0x%02X: %s", addrs[i], esp_err_to_name(w));
            i2c_master_bus_rm_device(dev);
        }
    }
    i2c_del_master_bus(bus);   // PCF8574 latches 0x00 → relays stay OFF after the bus closes
}
#endif

// ====== GLOBAL VARIABLES ======
unsigned long last_print = 0;     ///< legacy print timestamp (unused)
bool power_ready = false;         ///< legacy readiness flag (unused)
float last_voltage = 0.0;         ///< legacy last voltage, V (unused)
float last_frequency = 0.0;       ///< legacy last frequency, Hz (unused)
uint32_t last_cycle = 0;          ///< legacy cycle counter (unused)

rbgrid_unit_uid_t heater_sensor_uid;   ///< optional example "heater" unit (RBGRID_INVALID_UNIT_UID = not configured)

// ===== ADD GLOBAL VARIABLES =====
rbpower_electrical_network_t home_network;   ///< the home electrical network built from the stored configuration
static rbpower_extended_config_t* g_extended_config = NULL;
/** Site nominal L-N voltage for consumers outside the rbpower component (ESP-NOW hub: P = V × I for
 * current-only nodes). Reads the live hardware config (hardware-config POST updates it), falls back to
 * 230 V when unset or outside 50..500 V.
 * @return Nominal voltage in V. */
extern "C" float rbgrid_nominal_voltage_v(void)
{
    if (g_extended_config && g_extended_config->base_hardware_config) {
        float v = g_extended_config->base_hardware_config->network.nominal_voltage;
        if (v >= 50.0f && v <= 500.0f) return v;
    }
    return 230.0f;
}
static rbgrid_json_context_t* g_json_ctx = NULL;

bool rbhome_initialized = false;   ///< legacy flag (unused)

// ====== CALLBACKS FOR RBWIFI ======
// Callback for WiFi events
// MQTT topics/ids derived once from the STA MAC: client-id "rbgrid-<mac>",
// telemetry topic "rbgrid/<mac>/telemetry", status/LWT "rbgrid/<mac>/status".
static char s_mqtt_client_id[32]  = {0};
static char s_mqtt_topic[48]      = {0};
static char s_mqtt_lwt_topic[48]  = {0};
static char s_mqtt_cmd_topic[48]  = {0};
static char s_mqtt_resp_topic[56] = {0};

static bool s_mqtt_started = false;

/** Effective MQTT broker URI: the runtime-configured value persisted by
 *  POST /api/system/mqtt-config (littlefs /mqtt_broker), else the compile-time default. Apply-on-restart. */
static const char* rbgrid_effective_mqtt_broker(void)
{
    static char uri[160];
    size_t act = 0;
    if (littlefs_raw_exists("/mqtt_broker") &&
        littlefs_raw_read("/mqtt_broker", uri, sizeof(uri) - 1, &act) == ESP_OK && uri[0]) {
        uri[sizeof(uri) - 1] = 0;
        return uri;
    }
    return RBGRID_MQTT_BROKER_URI;
}

/** Build the mac-derived MQTT identifiers ONCE. Idempotent + safe to call before WiFi connects
 * (esp_read_mac reads efuse, no link needed) — so start_webserver() can build the telemetry config
 * with the canonical "rbgrid/<mac>/telemetry" topic instead of the non-mac fallback. Entity model:
 * identity/transport = mac; per-device topics rbgrid/<mac>/{telemetry,status,cmd}. */
static void rbgrid_build_mqtt_ids(void)
{
    if (s_mqtt_topic[0]) return;   // already built
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char dev[13];
    snprintf(dev, sizeof(dev), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_mqtt_client_id, sizeof(s_mqtt_client_id), "rbgrid-%s", dev);
    snprintf(s_mqtt_topic, sizeof(s_mqtt_topic), "rbgrid/%s/telemetry", dev);
    snprintf(s_mqtt_lwt_topic, sizeof(s_mqtt_lwt_topic), "rbgrid/%s/status", dev);
    snprintf(s_mqtt_cmd_topic, sizeof(s_mqtt_cmd_topic), "rbgrid/%s/cmd", dev);
    snprintf(s_mqtt_resp_topic, sizeof(s_mqtt_resp_topic), "rbgrid/%s/cmd/resp", dev);
}

static SemaphoreHandle_t s_ha_discovery_sem = NULL;

/** HA Discovery publish runs OFF the MQTT event task — the ~44-publish + cJSON burst in the event
 * handler would trip the task-WDT → boot loop. This task blocks on a semaphore the
 * connect callback gives, then does the paced publish with an ample cJSON stack. */
static void ha_discovery_task(void* arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_ha_discovery_sem, portMAX_DELAY) == pdTRUE) {
            if (rbmqtt_is_connected()) {
                rbgrid_json_publish_ha_discovery();
                esp_now_hub_publish_ha_nodes();   // per-node "Online" connectivity sensors
            }
        }
    }
}

/** Registered as the rbmqtt connect cb → runs in the MQTT event task, so it ONLY signals (never
 * publishes/blocks there). The discovery task does the work. Re-fires on every (re)connect. */
static void ha_discovery_on_connect(void)
{
    if (s_ha_discovery_sem) {
        xSemaphoreGive(s_ha_discovery_sem);
    }
}

/** @brief Strong definition of the component's weak hook (rbjson.h): unit create/rebind/rename ask for a
 * republish; same semaphore → same paced task, so the HTTP handler never publishes itself. */
extern "C" void rbgrid_ha_discovery_request(void)
{
    ha_discovery_on_connect();
}

#define OTA_CONFIRM_GRACE_S 60   ///< uptime on the network (s) before a fresh OTA image is marked valid
/** Mark a fresh OTA image valid after OTA_CONFIRM_GRACE_S, then release a deferred stats migration. */
static void ota_confirm_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(OTA_CONFIRM_GRACE_S * 1000));
    esp_err_t mv = esp_ota_mark_app_valid_cancel_rollback();
    if (mv == ESP_OK) {
        ESP_LOGI(TAG, "OTA image marked VALID (rollback cancelled)");
        rbpower_diag_printf("ota: new image confirmed valid (rollback cancelled)");
    } else {
        rbpower_diag_printf("FAIL ota: confirm failed: %s", esp_err_to_name(mv));
    }
    rbstats_migrate_notify_image_valid();
    vTaskDelete(NULL);
}

/** Boot: did the bootloader roll back a fresh image? esp_ota_get_last_invalid_partition() stays set until
 * the next OTA, so report each rejected image once (its ELF sha remembered in NVS). */
static void ota_report_rollback(void)
{
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    if (!bad) return;
    esp_app_desc_t d;
    if (esp_ota_get_partition_description(bad, &d) != ESP_OK) return;
    char sha[17];
    for (int i = 0; i < 8; i++) sprintf(&sha[i * 2], "%02x", d.app_elf_sha256[i]);
    nvs_handle_t h;
    if (nvs_open("rbota", NVS_READWRITE, &h) != ESP_OK) return;
    char seen[17] = {0}; size_t n = sizeof(seen);
    if (nvs_get_str(h, "rb_seen", seen, &n) != ESP_OK || strcmp(seen, sha) != 0) {
        const esp_partition_t *run = esp_ota_get_running_partition();
        rbpower_diag_printf("FAIL ota: ROLLED BACK — image in '%s' (elf %s) did not confirm; running '%s'",
                            bad->label, sha, run ? run->label : "?");
        nvs_set_str(h, "rb_seen", sha);
        nvs_commit(h);
    }
    nvs_close(h);
}

/**
 * @brief rbwifi status callback: event-log line on link changes, OTA confirm, mDNS, MQTT + HA discovery start.
 * @param[in] status     New Wi-Fi status.
 * @param[in] user_data  Unused.
 */
void wifi_event_callback(rbwifi_status_t status, void* user_data)
{
    ESP_LOGI(TAG, "WiFi status changed: %s", rbwifi_status_to_string(status));

    // OTA rollback confirm: reaching STA connectivity means a freshly-OTA'd image booted + initialized
    // + got on the network — a strong "this image is good" signal. Mark it valid so the bootloader
    // won't revert on the next reset. Idempotent + a no-op unless the running image is PENDING_VERIFY
    // (i.e. just OTA'd). A bad OTA that boot-loops / crashes on init / can't connect never reaches here
    // → auto-rollback to the previous partition. (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE.)
    {   /* event log: link state changes only (the callback repeats while reconnecting) */
        static int s_last_wifi = -1;
        if ((int)status != s_last_wifi) {
            s_last_wifi = (int)status;
            rbpower_diag_printf("wifi %s", rbwifi_status_to_string(status));
        }
    }
    if (status == RBWIFI_STATUS_CONNECTED) {
        esp_ota_img_states_t ost;
        bool pending = esp_ota_get_state_partition(esp_ota_get_running_partition(), &ost) == ESP_OK &&
                       ost == ESP_OTA_IMG_PENDING_VERIFY;
        if (pending) {
            /* Fresh OTA image: confirm only after OTA_CONFIRM_GRACE_S of uptime on the network — an image that
             * reaches Wi-Fi but crashes/reboots within that window still rolls back (OTA-only hubs have no
             * cable to recover a bad image). One confirm task per boot. */
            static bool s_confirm_started = false;
            if (!s_confirm_started) {
                rbpower_diag_printf("ota: new image on network — confirming in %ds unless it resets", OTA_CONFIRM_GRACE_S);
                /* a failed create would leave a good image unconfirmed → rolled back at the next reset:
                 * retry on the next Wi-Fi event instead of marking it started */
                s_confirm_started = xTaskCreate(ota_confirm_task, "ota_confirm", 3072, NULL, 3, NULL) == pdPASS;
                if (!s_confirm_started) rbpower_diag_printf("FAIL ota: confirm task not created (low RAM) — retry on the next Wi-Fi event");
            }
        } else {
            esp_ota_mark_app_valid_cancel_rollback();   /* no-op unless pending; kept for symmetry */
            // A deferred stats-format migration waits for exactly this confirmation (never touch
            // the old-format data while a rollback to the old fw is still possible).
            rbstats_migrate_notify_image_valid();
        }
    }

    // mDNS discovery: rbgrid-<mac4>.local + _http._tcp +
    // _rbgrid._tcp TXT{fw,api,https} — mobile wrappers and the web app find the hub without a manual IP.
    if (status == RBWIFI_STATUS_CONNECTED) {
        static bool s_mdns_started = false;
        if (!s_mdns_started && mdns_init() == ESP_OK) {
            s_mdns_started = true;
            uint8_t mac[6] = {0};
            esp_read_mac(mac, ESP_MAC_WIFI_STA);
            char host[16];
            snprintf(host, sizeof(host), "rbgrid-%02x%02x", mac[4], mac[5]);
            mdns_hostname_set(host);
            mdns_instance_name_set("RBgrid Power Monitor");
            mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
            const esp_app_desc_t *ad = esp_app_get_description();
            mdns_txt_item_t txt[] = {
                { "fw",    (ad && ad->version[0]) ? ad->version : "0.0.0" },
                { "api",   "1" },      /* RBGRID_API_VERSION */
                { "https", "0" },      /* flips with the HTTPS rollout */
            };
            mdns_service_add(NULL, "_rbgrid", "_tcp", 80, txt, sizeof(txt) / sizeof(txt[0]));
            ESP_LOGI(TAG, "mDNS up: %s.local (_http._tcp + _rbgrid._tcp)", host);
        }
    }

    // Bring MQTT up once we have STA connectivity (a broker is unreachable in AP-only).
    if (status == RBWIFI_STATUS_CONNECTED && !rbmqtt_is_connected() && !s_mqtt_started) {
        rbgrid_build_mqtt_ids();   // idempotent (start_webserver already built them for the tele config)
        s_mqtt_started = true;
        const char* broker = rbgrid_effective_mqtt_broker();   // persisted (mqtt-config endpoint) or default
        esp_err_t merr = rbmqtt_init(broker, s_mqtt_client_id, s_mqtt_lwt_topic);
        ESP_LOGI(TAG, "MQTT init (%s, topic=%s): %s",
                 broker, s_mqtt_topic, esp_err_to_name(merr));
        // HA MQTT Discovery: (re)publish retained entity configs on every broker (re)connect so HA
        // auto-creates devices/entities and re-discovers after an HA restart.
        // The publish runs on a DEDICATED task (the connect cb only signals it) — doing the ~44-publish
        // burst in the MQTT event task would trip the task-WDT (boot loop).
        if (!s_ha_discovery_sem) {
            s_ha_discovery_sem = xSemaphoreCreateBinary();
            xTaskCreate(ha_discovery_task, "ha_disc", 6144, NULL, tskIDLE_PRIORITY + 1, NULL);
        }
        rbmqtt_register_connect_cb(ha_discovery_on_connect);
        // Enable the bidirectional command/config channel: config is fully manageable over MQTT
        // (mirror of the HTTP config API) via rbgrid_command_dispatch. cmd → rbgrid/<dev>/cmd,
        // responses → rbgrid/<dev>/cmd/resp.
        if (merr == ESP_OK) {
            esp_err_t cerr = rbmqtt_enable_commands(s_mqtt_cmd_topic, s_mqtt_resp_topic,
                                                    rbgrid_command_dispatch);
            ESP_LOGI(TAG, "MQTT command channel (cmd=%s): %s",
                     s_mqtt_cmd_topic, esp_err_to_name(cerr));
        }
    }
}


/* ===== ESP-NOW management + health transport API (available in the code, not part of the v1.1.0 release) =====
 * The ESP-NOW hub lives in main/ (above rbpower), so its HTTP surface is registered here onto the running
 * server (rbwebserver_core_server). GET /api/v1/espnow = status+health (open). POST /api/v1/espnow =
 * {action, mac, seconds, ...} command (token-guarded). Thin facade over esp_now_hub_*. */
/** GET /api/v1/espnow: status + per-node health JSON (open). */
static esp_err_t espnow_http_get(httpd_req_t *req)
{
    add_cors_headers(req);   /* honors the dev_cors / allow-list CORS policy */
    httpd_resp_set_type(req, "application/json");
    char *js = esp_now_hub_status_json();
    if (!js) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status"); return ESP_FAIL; }
    httpd_resp_send(req, js, HTTPD_RESP_USE_STRLEN);
    free(js);
    return ESP_OK;
}
/** Parse a MAC given as 12 hex digits or colon-separated. */
static bool espnow_http_parse_mac(const char *s, uint8_t mac[6])
{
    return s && (sscanf(s, "%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx", &mac[0],&mac[1],&mac[2],&mac[3],&mac[4],&mac[5]) == 6 ||
                 sscanf(s, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", &mac[0],&mac[1],&mac[2],&mac[3],&mac[4],&mac[5]) == 6);
}
/** POST /api/v1/espnow {action: pair|latch|add|remove|disable|enable, mac, seconds, channels} (token-guarded). */
static esp_err_t espnow_http_post(httpd_req_t *req)
{
    add_cors_headers(req);   /* honors the dev_cors / allow-list CORS policy (+PNA on OPTIONS) */
    if (req->method == HTTP_OPTIONS) {
        httpd_resp_send(req, NULL, 0); return ESP_OK;
    }
    if (!rbwebserver_api_check_auth(req)) return ESP_FAIL;   /* 401 already sent */
    char body[256] = {0};
    int rl = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    if (rl > 0 && httpd_req_recv(req, body, rl) <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body"); return ESP_FAIL; }
    cJSON *j = cJSON_Parse(body);
    if (!j) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json"); return ESP_FAIL; }
    const cJSON *ja = cJSON_GetObjectItem(j, "action");
    const cJSON *jm = cJSON_GetObjectItem(j, "mac");
    const char *action = cJSON_IsString(ja) ? ja->valuestring : "";
    char resp[160]; resp[0] = 0; bool ok = true; uint8_t mac[6];
    httpd_resp_set_type(req, "application/json");
    if (!strcmp(action, "pair")) {
        const cJSON *js2 = cJSON_GetObjectItem(j, "seconds");
        uint32_t sec = cJSON_IsNumber(js2) ? (uint32_t)js2->valuedouble : 120;
        /* First node: ESP-NOW auto-starts only when the registry already has nodes — on a fresh hub
         * `pair` would answer {"error":"espnow off"} and the FIRST node could never be paired over
         * HTTP. Start the hub on demand here instead. */
        if (!esp_now_hub_running()) {
            esp_err_t se = esp_now_hub_start(0);
            if (se != ESP_OK) { ok = false; snprintf(resp, sizeof(resp), "{\"error\":\"espnow start failed: %s\"}", esp_err_to_name(se)); }
        }
        if (ok) { esp_now_hub_open_pairing(sec); snprintf(resp, sizeof(resp), "{\"ok\":true,\"pairing_open_s\":%u}", (unsigned)sec); }
    } else if (!strcmp(action, "latch")) {
        if (!esp_now_hub_running()) { ok = false; snprintf(resp, sizeof(resp), "{\"error\":\"espnow off\"}"); }
        else { uint32_t n = esp_now_hub_latch_all(0); snprintf(resp, sizeof(resp), "{\"ok\":true,\"latched\":%u}", (unsigned)n); }
    } else if (!strcmp(action, "remove") && cJSON_IsString(jm) && espnow_http_parse_mac(jm->valuestring, mac)) {
        uint32_t rm = esp_now_hub_delnode_cascade(mac);
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"removed_units\":%u}", (unsigned)rm);
    } else if ((!strcmp(action, "disable") || !strcmp(action, "enable")) && cJSON_IsString(jm) && espnow_http_parse_mac(jm->valuestring, mac)) {
        esp_now_hub_set_node_disabled(mac, !strcmp(action, "disable"));
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"disabled\":%s}", !strcmp(action, "disable") ? "true" : "false");
    } else if (!strcmp(action, "add") && cJSON_IsString(jm) && espnow_http_parse_mac(jm->valuestring, mac)) {
        espnow_reg_chan_t ch[ESPNOW_REG_MAX_CH]; uint8_t n = 0;
        const cJSON *jch = cJSON_GetObjectItem(j, "channels"), *e = NULL;
        if (cJSON_IsArray(jch)) cJSON_ArrayForEach(e, jch) {
            if (n >= ESPNOW_REG_MAX_CH) break;
            const cJSON *cc = cJSON_GetObjectItem(e, "ch"), *ct = cJSON_GetObjectItem(e, "ct_code");
            if (cJSON_IsNumber(cc) && cJSON_IsNumber(ct)) { ch[n].ch = (uint8_t)cc->valuedouble; ch[n].ct_code = (uint8_t)ct->valuedouble; n++; }
        }
        if (n == 0) { ok = false; snprintf(resp, sizeof(resp), "{\"error\":\"channels[] required\"}"); }
        else { esp_err_t e2 = esp_now_registry_add_node(mac, ch, n);
               ok = (e2 == ESP_OK); snprintf(resp, sizeof(resp), "{\"ok\":%s,\"reboot_to_register\":true,\"channels\":%u}", ok?"true":"false", n); }
    } else {
        ok = false; snprintf(resp, sizeof(resp), "{\"error\":\"action=pair|latch|add|remove|disable|enable + mac\"}");
    }
    cJSON_Delete(j);
    if (!ok) httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
/** Auto-start the ESP-NOW hub on boot once WiFi is associated, if the registry has nodes. Fixes the recurring
 * gotcha where `espnow on` issued before WiFi-ready returned "ok" but left running=0 (no beacons → nodes can't
 * acquire). A configured hub (≥1 registered node) now brings ESP-NOW up itself — important after an OTA reboot
 * so remote nodes reconnect without a manual verb. One-shot; a manual `espnow off` later still wins. */
static void espnow_autostart_task(void *arg)
{
    wifi_ap_record_t ap;
    for (int i = 0; i < 120 && esp_wifi_sta_get_ap_info(&ap) != ESP_OK; i++)
        vTaskDelay(pdMS_TO_TICKS(500));   /* up to ~60s for STA association (hub channel = router channel) */
    espnow_registry_t reg;
    if (!esp_now_hub_running() && esp_now_registry_load(&reg) && reg.node_count > 0) {
        esp_err_t e = esp_now_hub_start(0);
        ESP_LOGW(TAG, "ESP-NOW auto-start (registry %d node(s)): %s, running=%d",
                 reg.node_count, esp_err_to_name(e), (int)esp_now_hub_running());
    }
    vTaskDelete(NULL);
}

/** GET /api/system/provisioning: onboarding/provisioning progress for the web app's resumable
 * provisioning state-machine, in one GET (vs probing 8 endpoints). Lives in main/ (touches the ESP-NOW
 * registry + units + rbtime + token + https across layers). Read-only / open. */
static esp_err_t provisioning_http_get(httpd_req_t *req)
{
    add_cors_headers(req);   /* honors the dev_cors / allow-list CORS policy */
    httpd_resp_set_type(req, "application/json");
    wifi_ap_record_t ap;
    bool wifi_ok = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
    bool time_ok = RBTIME_IS_SYNCED();
    bool hw_ok   = littlefs_raw_exists("/hardware.json");
    int  units   = rbpower_home_active_unit_count();
    int  nodes   = 0; { espnow_registry_t r; if (esp_now_registry_load(&r)) nodes = r.node_count; }
    const rbpower_tariff_plan_t *tp = rbpower_tariff_get_current_plan();
    /* the built-in demo plan is not "tariff set" (otherwise the setup wizard would tick Tariff ✓ off it) */
    bool tariff_ok = tp && tp->active_zones_count > 0 && !rbpower_tariff_plan_is_placeholder(tp->plan_name, tp->provider_name);
    char tk[48] = {0}; size_t act = 0;
    bool token_ok = littlefs_raw_exists("/api_token") &&
                    littlefs_raw_read("/api_token", tk, sizeof(tk) - 1, &act) == ESP_OK && tk[0];
    bool https_ok = rbwebserver_get_https();
    bool ready = wifi_ok && time_ok && units > 0;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "wifi_provisioned", wifi_ok);
    cJSON_AddBoolToObject(r, "time_synced", time_ok);
    cJSON_AddBoolToObject(r, "hardware_configured", hw_ok);
    cJSON_AddNumberToObject(r, "units_bound", units);
    cJSON_AddNumberToObject(r, "nodes_paired", nodes);
    cJSON_AddBoolToObject(r, "tariff_set", tariff_ok);
    cJSON_AddBoolToObject(r, "token_set", token_ok);
    cJSON_AddBoolToObject(r, "https_enabled", https_ok);
    cJSON_AddBoolToObject(r, "device_ready", ready);
    char *s = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (!s) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json"); return ESP_FAIL; }
    httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
    free(s);
    return ESP_OK;
}

/** Register GET/POST/OPTIONS /api/v1/espnow and GET /api/system/provisioning on the running server. */
static void register_espnow_http(void)
{
    httpd_handle_t srv = rbwebserver_core_server();
    if (!srv) { ESP_LOGW(TAG, "espnow API: no server handle"); return; }
    httpd_uri_t g = { .uri = "/api/v1/espnow", .method = HTTP_GET,  .handler = espnow_http_get,  .user_ctx = NULL };
    httpd_uri_t p = { .uri = "/api/v1/espnow", .method = HTTP_POST, .handler = espnow_http_post, .user_ctx = NULL };
    httpd_uri_t o = { .uri = "/api/v1/espnow", .method = HTTP_OPTIONS, .handler = espnow_http_post, .user_ctx = NULL };
    /* Register FAILURES must be loud: on a full URI table (max_uri_handlers) these silently fall off and
     * the CORS OPTIONS wildcard turns them into mystery 405s. */
    esp_err_t eg = httpd_register_uri_handler(srv, &g);
    esp_err_t ep = httpd_register_uri_handler(srv, &p);
    esp_err_t eo = httpd_register_uri_handler(srv, &o);
    httpd_uri_t prov = { .uri = "/api/system/provisioning", .method = HTTP_GET, .handler = provisioning_http_get, .user_ctx = NULL };
    esp_err_t ev = httpd_register_uri_handler(srv, &prov);
    if (eg != ESP_OK || ep != ESP_OK || eo != ESP_OK || ev != ESP_OK) {
        ESP_LOGE(TAG, "espnow/provisioning URI registration FAILED (g=%s p=%s o=%s prov=%s) — "
                      "check max_uri_handlers", esp_err_to_name(eg), esp_err_to_name(ep),
                 esp_err_to_name(eo), esp_err_to_name(ev));
    }
    rbgrid_register_espnow_status_provider(esp_now_hub_status_json);   // MQTT `cmd get espnow` → status JSON
    ESP_LOGI(TAG, "main-side API registered: GET/POST /api/v1/espnow + GET /api/system/provisioning");
}

/** Start telemetry (MQTT + WebSocket channels), the web server, the WebSocket maintenance task and the
 *  main-side HTTP APIs (ESP-NOW, events, module firmware, I2C), plus the ESP-NOW auto-start task. */
static void start_webserver(void)
{
    // Build the mac-derived MQTT topics BEFORE the telemetry config so the MQTT channel publishes to
    // the canonical "rbgrid/<mac>/telemetry" (otherwise s_mqtt_topic is still empty here and the
    // channel falls back to the non-mac topic).
    rbgrid_build_mqtt_ids();

    // Load the persisted telemetry profile (lean/full) so the live push uses it from boot
    // (POST /api/system/telemetry-config persists it; lean drops lifetime/economics → CPU savings).
    {
        char prof[8] = {0};
        size_t act = 0;
        if (littlefs_raw_exists("/tele_profile") &&
            littlefs_raw_read("/tele_profile", prof, sizeof(prof) - 1, &act) == ESP_OK &&
            strncmp(prof, "lean", 4) == 0) {
            rbgrid_telemetry_set_lean(true);
            ESP_LOGI(TAG, "Telemetry profile: LEAN (persisted)");
        }
    }
    {   /* Persisted telemetry frame version. v2 is the default (since v1.1.0); a hub stays
         * on the legacy v1 frame only when its user chose it explicitly (POST telemetry-config {"frame":1}) */
        char fr[4] = {0}; size_t act = 0;
        if (littlefs_raw_exists("/tele_frame") &&
            littlefs_raw_read("/tele_frame", fr, sizeof(fr) - 1, &act) == ESP_OK && fr[0] == '1') {
            rbgrid_telemetry_set_frame(1);
            ESP_LOGI(TAG, "Telemetry frame: v1 (persisted user choice)");
        }
    }

    // Telemetry initialization
    rbpower_telemetry_config_t telemetry_config = {
        .telemetry_enabled = true,
        .json_buffer_size = 4096,
        .compact_json = true,
        .channels = {
            [RBPOWER_TELEMETRY_MQTT] = {
                .enabled = true,
                .send_interval_ms = 3000,
                .config = { .mqtt = {
                    .broker_url = RBGRID_MQTT_BROKER_URI,
                    .topic = s_mqtt_topic[0] ? s_mqtt_topic : "rbgrid/telemetry",
                    .qos = 0
                }}
            },
            [RBPOWER_TELEMETRY_HTTP] = { .enabled = false },
            [RBPOWER_TELEMETRY_WEBSOCKET] = {
                .enabled = true,
                .send_interval_ms = 3000
            }
        }
    };
    esp_err_t telemetry_err = rbpower_telemetry_init(&telemetry_config);
    if (telemetry_err != ESP_OK) {
        ESP_LOGE(TAG, "Telemetry initialization error: %s", esp_err_to_name(telemetry_err));
    } else {
        ESP_LOGI(TAG, "Telemetry initialized");
    }
    
    // WebSocket telemetry initialization
    telemetry_err = rbwebsocket_telemetry_init();
    if (telemetry_err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket telemetry initialization error: %s", esp_err_to_name(telemetry_err));
    }

    // Web server initialization
    rbwebserver_config_t config = RBWEBSERVER_DEFAULT_CONFIG();
    config.extended_config = g_extended_config;
    config.json_context = g_json_ctx;
    rbpower_err_t err = rbwebserver_init(&config);
    if (err != RBPOWER_OK) {
        ESP_LOGE(TAG, "Web server initialization error: %d", err);
        return;
    }
    
    // Configure redirects
    rbwebserver_redirect_config_t redirect_config = RBWEBSERVER_DEFAULT_REDIRECT_CONFIG();
    rbwebserver_configure_redirects(&redirect_config);
    
     
    // Start server — read the persisted HTTPS flag first so start() picks HTTP vs TLS.
    rbwebserver_load_https_flag();
    err = rbwebserver_start();
    if (err == RBPOWER_OK) {
        ESP_LOGI(TAG, "Web server started successfully");
        ESP_LOGI(TAG, "HTTP: /system-config");
        ESP_LOGI(TAG, "WebSocket: /ws/telemetry");
        
        // Create WebSocket maintenance task
        xTaskCreate(rbwebsocket_maintenance_task, "ws_maintenance", 4096, NULL, 5, NULL);
        ESP_LOGI(TAG, "WebSocket maintenance task started");

        register_espnow_http();   // ESP-NOW management + health API onto the running server
        rbevlog_register_http();  // GET /api/v1/system/events (+ POST /api/v1/debug/cmd on debug builds)
        modfw_register_http();    // STD module firmware update API (main/modfw.c)
        rbi2c_register_http();    // I2C buses / scan / module address API (main/rbi2c_api.c)
        xTaskCreate(espnow_autostart_task, "espnow_autostart", 4096, NULL, 4, NULL);  // auto-start hub when WiFi ready
    } else {
        ESP_LOGE(TAG, "Web server start error: %d", err);
    }
}

/** Stop the web server (unused). */
static void stop_webserver(void)
{
    rbwebserver_stop();
    ESP_LOGI(TAG, "Web server stopped");
}

/**
 * @brief Test of the period-history API (rbgrid_get_period_history()).
 *
 * Reads the history of unit 0x0004 for every period and logs a summary:
 * - last 48 hours (HOUR)
 * - last 14 days (DAY)
 * - last 8 weeks (WEEK)
 * - last 12 months (MONTH)
 * - last 5 years (YEAR)
 *
 * Debug aid; its call in app_main is commented out (kept for manual testing).
 */
static void test_period_statistics_api(void)
{
    ESP_LOGI("API_TEST", "╔═══════════════════════════════════════════════════════════════╗");
    ESP_LOGI("API_TEST", "║  PERIOD STATISTICS API TEST (rbgrid_get_period_history)        ║");
    ESP_LOGI("API_TEST", "╚═══════════════════════════════════════════════════════════════╝");

    // Unit under test
    const rbpower_unit_id_t test_unit = 0x0004;

    // ═══════════════════════════════════════════════════════════════════════════════
    // TEST 1: last 48 hours (HOUR period)
    // ═══════════════════════════════════════════════════════════════════════════════
    ESP_LOGI("API_TEST", "");
    ESP_LOGI("API_TEST", "─────────────────────────────────────────────────────────────");
    ESP_LOGI("API_TEST", "TEST 1: last 48 hours (HOUR period)");
    ESP_LOGI("API_TEST", "─────────────────────────────────────────────────────────────");

    const uint32_t hour_count = 48;
    rbpower_period_statistics_t* hour_stats = (rbpower_period_statistics_t*)malloc(
        hour_count * sizeof(rbpower_period_statistics_t));

    if (hour_stats) {
        uint32_t actual_count = 0;
        rbgrid_err_t err = rbgrid_get_period_history(
            test_unit,
            RBPOWER_STAT_PERIOD_1HOUR,
            0,  // from the start of the buffer (oldest first)
            hour_count,
            hour_stats,
            &actual_count
        );

        if (err == RBGRID_OK) {
            ESP_LOGI("API_TEST", "✅ Read %lu records HOUR", (unsigned long)actual_count);

            // Totals
            float total_consumed = 0.0f;
            float total_supplied = 0.0f;
            float avg_voltage = 0.0f;
            uint32_t valid_records = 0;

            for (uint32_t i = 0; i < actual_count; i++) {
                total_consumed += hour_stats[i].energy_period.consumed_wh;
                total_supplied += hour_stats[i].energy_period.supplied_wh;
                if (hour_stats[i].voltage_rms > 0.0f) {
                    avg_voltage += hour_stats[i].voltage_rms;
                    valid_records++;
                }
            }

            if (valid_records > 0) {
                avg_voltage /= valid_records;
            }

            ESP_LOGI("API_TEST", "  ├─ Total consumed: %.2f Wh", total_consumed);
            ESP_LOGI("API_TEST", "  ├─ Total supplied: %.2f Wh", total_supplied);
            ESP_LOGI("API_TEST", "  ├─ Average V_rms: %.2f V (from %lu valid records)",
                     avg_voltage, (unsigned long)valid_records);
            ESP_LOGI("API_TEST", "  └─ Time span: %llu - %llu us",
                     (unsigned long long)hour_stats[0].period_start_us,
                     (unsigned long long)hour_stats[actual_count - 1].period_end_us);

            // Show the first 3 and the last 3 records
            ESP_LOGI("API_TEST", "");
            ESP_LOGI("API_TEST", "  First 3 records:");
            for (uint32_t i = 0; i < 3 && i < actual_count; i++) {
                ESP_LOGI("API_TEST", "    [%lu] V=%.1f, consumed=%.2f Wh, supplied=%.2f Wh, completeness=%d%%",
                         (unsigned long)i,
                         hour_stats[i].voltage_rms,
                         hour_stats[i].energy_period.consumed_wh,
                         hour_stats[i].energy_period.supplied_wh,
                         hour_stats[i].data_completeness_percent);
            }

            ESP_LOGI("API_TEST", "  Last 3 records:");
            uint32_t start_idx = (actual_count > 3) ? (actual_count - 3) : 0;
            for (uint32_t i = start_idx; i < actual_count; i++) {
                ESP_LOGI("API_TEST", "    [%lu] V=%.1f, consumed=%.2f Wh, supplied=%.2f Wh, completeness=%d%%",
                         (unsigned long)i,
                         hour_stats[i].voltage_rms,
                         hour_stats[i].energy_period.consumed_wh,
                         hour_stats[i].energy_period.supplied_wh,
                         hour_stats[i].data_completeness_percent);
            }

        } else {
            ESP_LOGE("API_TEST", "❌ Read error HOUR: %d", err);
        }

        free(hour_stats);
    } else {
        ESP_LOGE("API_TEST", "❌ Out of memory for %lu HOUR records",
                 (unsigned long)hour_count);
    }

    // ═══════════════════════════════════════════════════════════════════════════════
    // TEST 2: last 14 days (DAY period)
    // ═══════════════════════════════════════════════════════════════════════════════
    ESP_LOGI("API_TEST", "");
    ESP_LOGI("API_TEST", "─────────────────────────────────────────────────────────────");
    ESP_LOGI("API_TEST", "TEST 2: last 14 days (DAY period)");
    ESP_LOGI("API_TEST", "─────────────────────────────────────────────────────────────");

    const uint32_t day_count = 14;
    rbpower_period_statistics_t* day_stats = (rbpower_period_statistics_t*)malloc(
        day_count * sizeof(rbpower_period_statistics_t));

    if (day_stats) {
        uint32_t actual_count = 0;
        rbgrid_err_t err = rbgrid_get_period_history(
            test_unit,
            RBPOWER_STAT_PERIOD_DAILY,
            0,
            day_count,
            day_stats,
            &actual_count
        );

        if (err == RBGRID_OK) {
            ESP_LOGI("API_TEST", "✅ Read %lu records DAY", (unsigned long)actual_count);

            float total_consumed = 0.0f;
            float total_supplied = 0.0f;

            for (uint32_t i = 0; i < actual_count; i++) {
                total_consumed += day_stats[i].energy_period.consumed_wh;
                total_supplied += day_stats[i].energy_period.supplied_wh;
            }

            ESP_LOGI("API_TEST", "  ├─ Total consumed: %.2f Wh (%.3f kWh)",
                     total_consumed, total_consumed / 1000.0f);
            ESP_LOGI("API_TEST", "  └─ Total supplied: %.2f Wh (%.3f kWh)",
                     total_supplied, total_supplied / 1000.0f);

            // Show every daily record
            ESP_LOGI("API_TEST", "");
            ESP_LOGI("API_TEST", "  All daily records:");
            for (uint32_t i = 0; i < actual_count; i++) {
                ESP_LOGI("API_TEST", "    Day [%lu]: consumed=%.2f Wh, supplied=%.2f Wh, cost=%.2f EUR",
                         (unsigned long)i,
                         day_stats[i].energy_period.consumed_wh,
                         day_stats[i].energy_period.supplied_wh,
                         day_stats[i].result_base.cost_consumed);
            }

        } else {
            ESP_LOGE("API_TEST", "❌ Read error DAY: %d", err);
        }

        free(day_stats);
    } else {
        ESP_LOGE("API_TEST", "❌ Out of memory for DAY");
    }

    // ═══════════════════════════════════════════════════════════════════════════════
    // TEST 3: last 8 weeks (WEEK period)
    // ═══════════════════════════════════════════════════════════════════════════════
    ESP_LOGI("API_TEST", "");
    ESP_LOGI("API_TEST", "─────────────────────────────────────────────────────────────");
    ESP_LOGI("API_TEST", "TEST 3: last 8 weeks (WEEK period)");
    ESP_LOGI("API_TEST", "─────────────────────────────────────────────────────────────");

    const uint32_t week_count = 8;
    rbpower_period_statistics_t* week_stats = (rbpower_period_statistics_t*)malloc(
        week_count * sizeof(rbpower_period_statistics_t));

    if (week_stats) {
        uint32_t actual_count = 0;
        rbgrid_err_t err = rbgrid_get_period_history(
            test_unit,
            RBPOWER_STAT_PERIOD_WEEKLY,
            0,
            week_count,
            week_stats,
            &actual_count
        );

        if (err == RBGRID_OK) {
            if (actual_count > 0) {
                ESP_LOGI("API_TEST", "✅ Read %lu records WEEK", (unsigned long)actual_count);

                for (uint32_t i = 0; i < actual_count; i++) {
                    ESP_LOGI("API_TEST", "    Week [%lu]: consumed=%.2f Wh, supplied=%.2f Wh",
                             (unsigned long)i,
                             week_stats[i].energy_period.consumed_wh,
                             week_stats[i].energy_period.supplied_wh);
                }
            } else {
                ESP_LOGW("API_TEST", "⚠️  No WEEK data yet (expected on a new device)");
            }
        } else {
            ESP_LOGE("API_TEST", "❌ Read error WEEK: %d", err);
        }

        free(week_stats);
    }

    // ═══════════════════════════════════════════════════════════════════════════════
    // TEST 4: last 12 months (MONTH period)
    // ═══════════════════════════════════════════════════════════════════════════════
    ESP_LOGI("API_TEST", "");
    ESP_LOGI("API_TEST", "─────────────────────────────────────────────────────────────");
    ESP_LOGI("API_TEST", "TEST 4: last 12 months (MONTH period)");
    ESP_LOGI("API_TEST", "─────────────────────────────────────────────────────────────");

    const uint32_t month_count = 12;
    rbpower_period_statistics_t* month_stats = (rbpower_period_statistics_t*)malloc(
        month_count * sizeof(rbpower_period_statistics_t));

    if (month_stats) {
        uint32_t actual_count = 0;
        rbgrid_err_t err = rbgrid_get_period_history(
            test_unit,
            RBPOWER_STAT_PERIOD_MONTHLY,
            0,
            month_count,
            month_stats,
            &actual_count
        );

        if (err == RBGRID_OK) {
            if (actual_count > 0) {
                ESP_LOGI("API_TEST", "✅ Read %lu records MONTH", (unsigned long)actual_count);

                for (uint32_t i = 0; i < actual_count; i++) {
                    ESP_LOGI("API_TEST", "    Month [%lu]: consumed=%.2f Wh, supplied=%.2f Wh, cost=%.2f EUR",
                             (unsigned long)i,
                             month_stats[i].energy_period.consumed_wh,
                             month_stats[i].energy_period.supplied_wh,
                             month_stats[i].result_base.cost_consumed);
                }
            } else {
                ESP_LOGW("API_TEST", "⚠️  No MONTH data yet (expected on a new device)");
            }
        } else {
            ESP_LOGE("API_TEST", "❌ Read error MONTH: %d", err);
        }

        free(month_stats);
    }

    // ═══════════════════════════════════════════════════════════════════════════════
    // TEST 5: last 5 years (YEAR period)
    // ═══════════════════════════════════════════════════════════════════════════════
    ESP_LOGI("API_TEST", "");
    ESP_LOGI("API_TEST", "─────────────────────────────────────────────────────────────");
    ESP_LOGI("API_TEST", "TEST 5: last 5 years (YEAR period)");
    ESP_LOGI("API_TEST", "─────────────────────────────────────────────────────────────");

    const uint32_t year_count = 5;
    rbpower_period_statistics_t* year_stats = (rbpower_period_statistics_t*)malloc(
        year_count * sizeof(rbpower_period_statistics_t));

    if (year_stats) {
        uint32_t actual_count = 0;
        rbgrid_err_t err = rbgrid_get_period_history(
            test_unit,
            RBPOWER_STAT_PERIOD_YEARLY,
            0,
            year_count,
            year_stats,
            &actual_count
        );

        if (err == RBGRID_OK) {
            if (actual_count > 0) {
                ESP_LOGI("API_TEST", "✅ Read %lu records YEAR", (unsigned long)actual_count);

                for (uint32_t i = 0; i < actual_count; i++) {
                    ESP_LOGI("API_TEST", "    Year [%lu]: consumed=%.2f Wh (%.3f kWh), supplied=%.2f Wh",
                             (unsigned long)i,
                             year_stats[i].energy_period.consumed_wh,
                             year_stats[i].energy_period.consumed_wh / 1000.0f,
                             year_stats[i].energy_period.supplied_wh);
                }
            } else {
                ESP_LOGW("API_TEST", "⚠️  No YEAR data yet (expected on a new device)");
            }
        } else {
            ESP_LOGE("API_TEST", "❌ Read error YEAR: %d", err);
        }

        free(year_stats);
    }

    ESP_LOGI("API_TEST", "");
    ESP_LOGI("API_TEST", "╔═══════════════════════════════════════════════════════════════╗");
    ESP_LOGI("API_TEST", "║             API TEST FINISHED                                  ║");
    ESP_LOGI("API_TEST", "╚═══════════════════════════════════════════════════════════════╝");
    ESP_LOGI("API_TEST", "");
}


/* NOTE (boot-recovery / GPIO12 VDD_SDIO strap): GPIO12 (MTDI) is BOTH the external NOR-flash MISO and the
 * ESP32 VDD_SDIO flash-voltage strap, so a warm reset (esp_restart/OTA) intermittently bricks (ROM mis-reads
 * the float). A firmware gpio_hold-the-strap mitigation was tried + REVERTED: it didn't reliably fix it AND
 * made recovery sticky (the RTC hold survives an RTS-reset → needs a VCC power-cycle). The real fix is the
 * flash-voltage eFuse (espefuse set_flash_voltage 3.3V → ROM ignores the strap), provisioned per device — see
 * the hardware documentation. No firmware change carries that. */
/* cJSON → PSRAM (under web-app load internal RAM dropped to a few dozen bytes free; 8 parallel HTTP clients
 * reproduce it). Every cJSON node/string is < 16 KB, so with
 * SPIRAM_MALLOC_ALWAYSINTERNAL=16384 they all landed in INTERNAL RAM — a 200-record history document is
 * thousands of nodes. JSON is never touched from ISRs/flash-cache-off code, so PSRAM is safe; fall back to
 * the default heap if PSRAM is exhausted. free() handles both (IDF heap_caps). */
#include "cJSON.h"
/** cJSON allocator: PSRAM first, default heap as fallback. */
static void *rbgrid_cjson_malloc(size_t sz)
{
    void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(sz);
}
/** cJSON deallocator (free() handles both heaps). */
static void rbgrid_cjson_free(void *p) { free(p); }

/** @def RBPOWER_RUN_INJECT_TESTS
 *  @brief 1 = run the external-source injection self-test at boot (opt-in, default 0; defined in app_main). */

/**
 * @brief Application entry point (ESP-IDF main task; stack 12288, pinned to CPU1 in sdkconfig).
 *
 * Boot order: cJSON hooks → event log → (debug-bridge build) NVS + event loop → Wi-Fi AP+STA → configuration
 * create + storage load → sensor/unit config (saved or default) → time config → rbAmp fleet units → ESP-NOW
 * node units → tariff plan → rbgrid/rbpower init → web server → rbAmp bridge start → event-log storage + boot
 * forensics → debug console verbs. In debug builds rbbench owns
 * UART0; otherwise the IDF console/ESP_LOG serves UART0.
 */
extern "C" void app_main(void) {
    { cJSON_Hooks h = { rbgrid_cjson_malloc, rbgrid_cjson_free }; cJSON_InitHooks(&h); }   /* before any JSON */
    rbevlog_init();   /* event log ring + diag log sink: before the first diag line */

#if defined(RBGRID_BENCH_RELAY_FAILSAFE)
    // SAFETY FIRST: drive the test-rig load-bank relays OFF before anything else (see above).
    bench_relays_failsafe_off();
#endif

#if RBGRID_RBBENCH && !RBGRID_WIFI
    /* System deps that normally run inside rbwifi_init() — which the debug-bridge build gates
     * out. NVS is required by config/storage/console paths; the default event loop by
     * component event handlers. Without these the boot cascades (storage disables,
     * console RX dies). The non-debug build keeps them in rbwifi_init. */
    {
        esp_err_t nvs_ret = nvs_flash_init();
        if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            nvs_ret = nvs_flash_init();
        }
        ESP_LOGI("MAIN", "NVS init (debug-bridge build): %s", esp_err_to_name(nvs_ret));
        esp_err_t evt_ret = esp_event_loop_create_default();
        ESP_LOGI("MAIN", "default event loop (debug-bridge build): %s", esp_err_to_name(evt_ret));
    }
#endif

    // NOTE: the flash module tests run after rbgrid_config_create(), after the flash init and before
    // rbgrid_init_system() (see the flash test block further down).

    // WIFI initialization through rbwifi
    ESP_LOGI(TAG, "=== rbpower WiFi AP+STA Test ===");
    
    // WiFi configuration
    rbwifi_config_t wifi_config = RBWIFI_DEFAULT_CONFIG();
    
    // Check for defined SSID and password
    {
        /* Credential order: NVS (the user's choice) first, compiled-in dev credentials (gitignored
         * wifi_secrets.h) only as a fallback for a blank device — so a binary built from a tree with
         * that file never ignores the user's Wi-Fi setup page. */
        char saved_ssid[RBWIFI_MAX_SSID_LEN] = {0};
        char saved_password[RBWIFI_MAX_PASSWORD_LEN] = {0};
        if (rbwifi_nvs_load_credentials(saved_ssid, saved_password) == RBPOWER_OK) {
            strncpy(wifi_config.ssid, saved_ssid, sizeof(wifi_config.ssid) - 1);
            strncpy(wifi_config.password, saved_password, sizeof(wifi_config.password) - 1);
            ESP_LOGI(TAG, "Using saved WiFi credentials from NVS: %s", saved_ssid);
        } else {
    #ifdef WIFI_SSID
            strncpy(wifi_config.ssid, WIFI_SSID, sizeof(wifi_config.ssid) - 1);
            strncpy(wifi_config.password, WIFI_PASS, sizeof(wifi_config.password) - 1);
            ESP_LOGI(TAG, "Using compiled-in DEV WiFi credentials: %s", WIFI_SSID);
    #else
            ESP_LOGI(TAG, "No WiFi credentials found, starting in AP mode only");
    #endif
        }
    }
    
    /* Per-device AP name: rbgrid-<last 4 hex of the STA MAC> — the same name as
     * the mDNS host, so a kit with several hubs shows which network is which hub. */
    {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(wifi_config.ap_ssid, sizeof(wifi_config.ap_ssid), "rbgrid-%02x%02x", mac[4], mac[5]);
    }

    /* AP password: the user's own (POST /api/system/ap) or the provisioned setup password */
    rbgrid_ap_drop_auto_password();
    {
        char pw[RBWIFI_MAX_PASSWORD_LEN] = {0};
        bool user = rbgrid_ap_user_password(pw, sizeof(pw));
        if (user) strlcpy(wifi_config.ap_password, pw, sizeof(wifi_config.ap_password));
        rbpower_diag_printf("wifi: AP password = %s", user ? "set by the user" : "setup password (the user should change it)");
    }

    // Enable AP+STA mode
    wifi_config.enable_sta = true;
    wifi_config.enable_ap = true;
    
    // WiFi successful initialization flag
    bool wifi_initialized = false;
    
    // WiFi module initialization — SKIPPED in the debug-bridge build to free ~40-50KB DRAM.
    // Its interface is rbbench/UART, not wifi/web (min heap was ~8KB after the first 15-min
    // stats fire with wifi+webserver up).
#if RBGRID_WIFI
    rbpower_err_t err = rbwifi_init(&wifi_config);
    if (err != RBPOWER_OK) {
        ESP_LOGE(TAG, "Failed to init WiFi: %d", err);
        ESP_LOGW(TAG, "Continuing without WiFi functionality");
    } else {
        // Register callbacks
        rbwifi_register_event_callback(wifi_event_callback, NULL);
        
        // Start WiFi
        err = rbwifi_start();
        if (err != RBPOWER_OK) {
            ESP_LOGE(TAG, "Failed to start WiFi: %d", err);
            ESP_LOGW(TAG, "WiFi features will not be available");
        } else {
            wifi_initialized = true;

#if RBGRID_RBBENCH
            // Debug build with Wi-Fi: disable wifi power-save (modem-sleep). It is independent of
            // CONFIG_PM_ENABLE and can gate peripheral clocks at idle, which is suspected to drift
            // the APB-clocked UART0 divisor → rbbench RX dies. Keep the radio always-on so the
            // debug console stays stable.
            esp_err_t ps_rc = esp_wifi_set_ps(WIFI_PS_NONE);
            ESP_LOGI(TAG, "rbbench: wifi power-save -> NONE: %s", esp_err_to_name(ps_rc));
#endif

            // Display AP mode information
            esp_netif_ip_info_t ap_info;
            if (rbwifi_get_ap_info(&ap_info) == RBPOWER_OK) {
                ESP_LOGI(TAG, "WiFi AP started: SSID=%s, password: %s", wifi_config.ap_ssid,
                         wifi_config.ssid[0] ? "this hub's own (console `appw`)" : "setup default");
                ESP_LOGI(TAG, "AP IP: " IPSTR, IP2STR(&ap_info.ip));
                ESP_LOGI(TAG, "Connect to http://" IPSTR "/wifi for configuration", IP2STR(&ap_info.ip));
            }
            
            // Check STA connection status
            rbwifi_info_t wifi_info;
            if (rbwifi_get_status(&wifi_info) == RBPOWER_OK && wifi_info.status == RBWIFI_STATUS_CONNECTED) {
                ESP_LOGI(TAG, "WiFi STA connected to: %s", wifi_info.ap_info.ssid);
                ESP_LOGI(TAG, "STA IP: " IPSTR, IP2STR(&wifi_info.ip_info.ip));
            } else {
                ESP_LOGI(TAG, "WiFi STA not connected yet");
            }
        }
    }
#else
    ESP_LOGW(TAG, "WiFi + webserver DISABLED (debug-bridge build) — RAM headroom");
#endif

#if !RBGRID_RBBENCH
    printf("\n=== RBgrid ===\n");
#endif

    // 1. Start-up banner
#if !RBGRID_RBBENCH
    printf("Init rbpower...\n");
#endif
    
    ESP_LOGI("MAIN", "=== rbpower + rbhome Full Access Test ===");
    

    
    // ===== NEW APPROACH: Using rbgrid_config_api =====
    // Create configuration through API
    rbgrid_config_handle_t config_handle = rbgrid_config_create("Test House", "HongKong");
    if (!config_handle) {
        ESP_LOGE(TAG, "Failed to create config via rbgrid_config_api");
        return;
    }
    
    // Get pointers to internal structures for additional configuration
    // TEMPORARY SOLUTION: direct access to structures until full API implementation
    rbgrid_config_context_t* config_ctx = (rbgrid_config_context_t*)config_handle;
    rbpower_extended_config_t* extended_config = config_ctx->extended_config;
    rbpower_hardware_config_t* base_hardware_config = config_ctx->hardware_config;
    
    // Additional settings not yet covered by API
    base_hardware_config->network.nominal_frequency = 50;  // Default 50Hz (hardware-config / saved config override it)
    
    // Initialize JSON module early for storage operations
    ESP_LOGI(TAG, "Initializing JSON module early for storage tests...");
    static rbgrid_json_context_t json_ctx = {};
    rbgrid_json_device_info_t json_info = {
        .device_id = "ESP32-RBGrid-001",
        .device_name = "RBGrid Storage Test",
        .location = "Test Location",
        .timezone = "UTC",
    };
    rbgrid_json_init(&json_ctx, &json_info);
    g_json_ctx = &json_ctx;
    ESP_LOGI(TAG, "JSON module initialized successfully");
    
    // Initialize configuration storage
    ESP_LOGI(TAG, "Initializing configuration storage...");
    esp_err_t storage_ret = rbgrid_storage_init();
    if (storage_ret == ESP_OK) {
        // Try to load existing configuration into global context
        if (rbgrid_storage_load_system(extended_config) == ESP_OK) {
            ESP_LOGI(TAG, "Configuration loaded from external Flash to global context");
        } else {
            ESP_LOGI(TAG, "No stored configuration found, saving current settings...");
            // Save current configuration as default
            if (rbgrid_storage_save_system(extended_config) == ESP_OK) {
                ESP_LOGI(TAG, "Default configuration saved to external Flash");
            }
        }
        
        // Check storage status
        rbgrid_storage_check_files();
        
        // Show storage statistics
        size_t total = 0, used = 0;
        if (rbgrid_storage_get_stats(&total, &used) == ESP_OK) {
            ESP_LOGI(TAG, "Storage: %zu KB used of %zu KB total", used/1024, total/1024);
        }
    } else {
        ESP_LOGW(TAG, "Configuration storage not available: %s", esp_err_to_name(storage_ret));
    }


    // === SENSOR + UNIT config: load-or-default-and-save ===
    // If the configurator (web app) saved BOTH the sensor layer (/hardware.json) AND the unit layer
    // (/units.json), restore them: load_hardware brings back the sensor channel definitions, load_units
    // recreates the units + supplies (by type/role, deriving sensor UIDs from the saved channels). The
    // hardcoded supply block below self-gates on the sensor UIDs — when loaded the UIDs stay INVALID
    // so that block is skipped (load_units already made the supplies). Otherwise (first boot) build
    // the hardcoded defaults; they're persisted after init (below) so the NEXT boot loads them.
    // NOTE: rbAmp units (add_rbamp_module, further down) are ALWAYS built from the fleet config — they
    // are not in units.json by design (load_units skips I2C), so they must not be gated. Filtering stays
    // separate (always). No hardcode-fallback on a partial load (would double-add).
    rbgrid_unit_uid_t voltage_sensor_uid = RBGRID_INVALID_UNIT_UID;
    rbgrid_unit_uid_t current_sensor_uid = RBGRID_INVALID_UNIT_UID;
    bool have_saved_hw_units = littlefs_raw_exists("/hardware.json") && littlefs_raw_exists("/units.json");
    if (have_saved_hw_units) {
        esp_err_t lh = rbgrid_storage_load_hardware(base_hardware_config);
        esp_err_t lu = rbgrid_storage_load_units(config_handle);
        ESP_LOGI(TAG, "✅ Sensor+unit config LOADED from flash (hardware:%s units:%s) — supplies recreated by load_units",
                 esp_err_to_name(lh), esp_err_to_name(lu));
    } else {
        ESP_LOGI(TAG, "=== No saved hardware+units — building DEFAULT sensors (will persist after init) ===");
        // Default voltage unit: legacy ADC-typed definition (ZMPT107-1, GPIO35) — no on-chip ADC is sampled,
        // it only binds the default unit that the rbAmp bridge feeds
        voltage_sensor_uid = rbgrid_config_add_adc_sensor(config_handle, GPIO_NUM_35, RBSENSOR_TYPE_VOLTAGE_ZMPT107_1);
        if (voltage_sensor_uid == RBGRID_INVALID_UNIT_UID) ESP_LOGE(TAG, "Failed to add voltage sensor");
        else ESP_LOGI(TAG, "Voltage sensor added with unit_UID: 0x%04X", voltage_sensor_uid);
        // Default main-supply current unit: legacy ADC-typed definition (SCT013-10A, GPIO34), fed by rbAmp
        current_sensor_uid = rbgrid_config_add_adc_sensor(config_handle, GPIO_NUM_34, RBSENSOR_TYPE_CURRENT_SCT013_10A);
        if (current_sensor_uid == RBGRID_INVALID_UNIT_UID) ESP_LOGE(TAG, "Failed to add main current sensor");
        else ESP_LOGI(TAG, "Main current sensor added with unit_UID: 0x%04X", current_sensor_uid);
    }
    
    /* The default alternative-supply (0x0005) and heater (0x0004) units are not created: only
     * 0x0006 (main_supply) / 0x0007 (voltage_bus) remain as defaults, fed by the rbAmp inject.
     * Further sources come from the rbAmp fleet (add_rbamp_module) and ESP-NOW nodes. */
    rbgrid_unit_uid_t alt_sensor_uid = RBGRID_INVALID_UNIT_UID;
    heater_sensor_uid = RBGRID_INVALID_UNIT_UID;
    int config_err = RBGRID_CONFIG_OK;

    // Filtering configuration (remains manual)
    base_hardware_config->adc_sensors.enable_filtering = false;
    base_hardware_config->adc_sensors.filter_samples = 1;
    
    // === TIME CONFIGURATION LOAD FROM FLASH ===
    ESP_LOGI(TAG, "=== Loading time configuration from Flash ===");

    // Try to load saved time configuration
    if (littlefs_raw_exists("/time.json")) {
        ESP_LOGI(TAG, "Found /time.json file, attempting to load...");

        esp_err_t time_load_ret = rbgrid_storage_load_time(config_handle);
        if (time_load_ret == ESP_OK) {
            ESP_LOGI(TAG, "✅ Time configuration loaded and applied from Flash");
            ESP_LOGI(TAG, "Time settings will override defaults from main.cpp");
        } else if (time_load_ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Time configuration file not found, using defaults");
        } else {
            ESP_LOGE(TAG, "Failed to load time configuration: %s", esp_err_to_name(time_load_ret));
            ESP_LOGW(TAG, "Continuing with default time settings");
        }
    } else {
        ESP_LOGI(TAG, "No /time.json file found, using default time configuration");
 
        // Time configuration through API functions
        /* Fresh device: UTC until the user sets a zone (setup / time-sync save_config). The default must be a
         * POSIX TZ string with an offset ("UTC0"): a name without an offset is rejected by the timezone
         * validator ("timezone rejected" on every new hub). */
        config_err = rbgrid_config_set_sntp_time(config_handle,
                                                "pool.ntp.org",
                                                "time.nist.gov",
                                                "UTC0");
        if (config_err != RBGRID_CONFIG_OK) {
            ESP_LOGW(TAG, "Failed to configure SNTP time: %d", config_err);
        } else {
            ESP_LOGI(TAG, "✅ SNTP time configuration completed");
        }
        
        // Time fallback configuration
        config_err = rbgrid_config_set_time_fallback(config_handle,
                                                    true,   // enable RTC fallback
                                                    true,   // enable manual fallback
                                                    1704067020); // January 1, 2024, 07:57:00 UTC = December 31, 23:57 local (UTC-8)
                                                                 // → 3 min later DAY + WEEK(Mon) + MONTH(1st) + YEAR(Jan 1) all roll over at once
        if (config_err != RBGRID_CONFIG_OK) {
            ESP_LOGW(TAG, "Failed to configure time fallback: %d", config_err);
        }
        
        // Advanced time settings
        config_err = rbgrid_config_set_time_advanced(config_handle,
                                                    3600,   // sync every hour
                                                    10000,  // 10 second timeout
                                                    3,      // 3 retry attempts
                                                    true);  // enable DST
        if (config_err != RBGRID_CONFIG_OK) {
            ESP_LOGW(TAG, "Failed to configure advanced time settings: %d", config_err);
        }
    }
    
    // ESP-NOW settings remain in direct access until API is available
    memset(base_hardware_config->time_config.esp_now_hub_mac, 0, 6);
    base_hardware_config->time_config.esp_now_timeout_ms = 1000;
    base_hardware_config->time_config.on_time_sync_status_changed = NULL;
    
    // External flash is now initialized automatically through rbpower_external_flash_init()
    // base_hardware_config->external_flash_config = &external_flash_config;  // DEPRECATED - now using consolidated Flash manager

    // Set Flash status flag for rbpower modules
    // Note: external_flash_is_available() checks both initialized AND otp_validated
    // For development/testing, we check only if config is mounted (Flash is physically working)
    if (external_flash_is_config_mounted()) {
        base_hardware_config->external_flash.status.initialized = true;
        ESP_LOGI(TAG, "External Flash status set: initialized = true (config mounted)");
    } else {
        base_hardware_config->external_flash.status.initialized = false;
        ESP_LOGW(TAG, "External Flash config not mounted, status.initialized = false");
    }

   
    // 1. Create Voltage Bus using voltage sensor unit_UID
    if (voltage_sensor_uid != RBGRID_INVALID_UNIT_UID) {
        config_err = rbgrid_config_set_voltage_bus(config_handle, voltage_sensor_uid, "Main Voltage Bus");
        if (config_err != RBGRID_CONFIG_OK) {
            ESP_LOGE(TAG, "Failed to configure voltage bus: %d", config_err);
        } else {
            ESP_LOGI(TAG, "✅ Voltage bus configured successfully with unit_UID: 0x%04X", voltage_sensor_uid);
        }
    }
    
    // 2. Create Main Supply using current sensor unit_UID
    if (current_sensor_uid != RBGRID_INVALID_UNIT_UID) {
        // Main supply = the primary grid feed → FULL statistics depth (3SEC..YEARLY incl. day/week/
        // month/year), matching the role intent RBPOWER_AUTO_PERIODS_BY_ROLE(MAIN_SUPPLY)=COMPLETE.
        // Main units need long-period history, so the depth is not capped at 1HOUR. NOTE: day/week+
        // records finalize on a wall-clock boundary hit.
        config_err = rbgrid_config_set_main_supply(config_handle, current_sensor_uid, "Main Supply Current", RBPOWER_STAT_PERIOD_YEARLY);
        if (config_err != RBGRID_CONFIG_OK) {
            ESP_LOGE(TAG, "Failed to configure main supply: %d", config_err);
        } else {
            ESP_LOGI(TAG, "✅ Main supply configured successfully with unit_UID: 0x%04X", current_sensor_uid);
        }
    }
    

    // Alternative Supply (0x0005) + Heater (0x0004) units are skipped while no source is
    // defined for them (see above) — no orphan units. Guarded so they can be re-enabled
    // cleanly when a source is assigned.
    if (alt_sensor_uid != RBGRID_INVALID_UNIT_UID) {
        // Alt supply = a primary source too → FULL depth like main supply (was 1HOUR). See note above.
        config_err = rbgrid_config_set_alternative_supply(config_handle, alt_sensor_uid, "Alternative Supply", RBPOWER_STAT_PERIOD_YEARLY);
        if (config_err != RBGRID_CONFIG_OK) {
            ESP_LOGE(TAG, "Failed to set alternative supply: %d", config_err);
        } else {
            ESP_LOGI(TAG, "✅ Alternative supply configured");
        }
    }

    if (heater_sensor_uid != RBGRID_INVALID_UNIT_UID) {
        config_err = rbgrid_config_add_unit(config_handle, heater_sensor_uid, "Heater 10A", RBPOWER_STAT_PERIOD_1HOUR);
        if (config_err != RBGRID_CONFIG_OK) {
            ESP_LOGE(TAG, "Failed to set Heater: %d", config_err);
        } else {
            ESP_LOGI(TAG, "✅ Heater configured");
        }
    }

#if RBGRID_RBAMP_FLEET
    /* rbAmp fleet: create external I2C-sourced default units for the extra current
     * modules I2(0x51) + I3(0x52). 0x50 (UI1) feeds the existing main_supply 0x0006 +
     * voltage_bus 0x0007. The rbAmp source (below) provisions CT + injects into these.
     * (RBgrid has no SCT-013-5A type → the 5A channel uses the 10A type for its unit
     * rating only; its CT code (1) is set correctly by the source config.) */
    {
        /* Create rbAmp units ONLY for channels whose persisted fleet binding is still set. A unit
         * deleted via DELETE /api/v1/units cleared its fleet entry's unit_id to 0
         * (rbamp_bridge_unbind_unit → saved /rbamp.json) → that channel is skipped here, so the unit
         * does NOT respawn on reboot. A fresh device (no /rbamp.json) → load fails → all channels
         * default-bound → full default fleet. NOTE: the bridge start further down independently loads
         * the same /rbamp.json, so a skipped channel is also not injected — the two stay consistent. */
        rbamp_bridge_config_t bound_fleet;
        bool have_fleet = (rbgrid_storage_load_rbamp(&bound_fleet) == ESP_OK);
#if !RBGRID_DEFAULT_BENCH_FLEET
        /* Product default: a fresh hub (no saved fleet) gets only module 0x50 (units 0x0006/0x0007). Modules 0x51 /
         * 0x52 get default units only when a saved fleet config binds their channels (hubs set up that way). */
        if (have_fleet)
#endif
        {
        rbgrid_rbamp_channel_t i2_full[] = {
            { 0, RBSENSOR_TYPE_CURRENT_SCT013_10A, "rbAmp I2 I0 (5A)" },
            { 1, RBSENSOR_TYPE_CURRENT_SCT013_10A, "rbAmp I2 I1 (10A)" },
        };
        rbgrid_rbamp_channel_t i3_full[] = {
            { 0, RBSENSOR_TYPE_CURRENT_SCT013_30A, "rbAmp I3 I0 (30A)" },
            { 1, RBSENSOR_TYPE_CURRENT_SCT013_20A, "rbAmp I3 I1 (20A)" },
        };
        rbgrid_rbamp_channel_t buf[RBAMP_MAX_ENTRIES];
        uint8_t n;

        /* Channels already restored from units.json (with their persisted identity — possibly
         * an opaque id + custom name/point) must NOT be re-created here with hardcoded defaults.
         * builder_has_source sees them by physics (module/ch), whatever their id. */
        n = 0;
        for (int c = 0; c < 2; ++c)
            if ((!have_fleet || rbamp_channel_bound(&bound_fleet, 1, i2_full[c].channel)) &&
                !rbgrid_config_builder_has_source(config_handle, RBPOWER_SOURCE_I2C_SENSOR,
                                                  1, i2_full[c].channel)) buf[n++] = i2_full[c];
        if (n > 0) rbgrid_config_add_rbamp_module(config_handle, 0x51, 1, buf, n);
        else ESP_LOGW(TAG, "rbAmp module 0x51: all channels unbound or already restored — no defaults created");

        n = 0;
        for (int c = 0; c < 2; ++c)
            if ((!have_fleet || rbamp_channel_bound(&bound_fleet, 2, i3_full[c].channel)) &&
                !rbgrid_config_builder_has_source(config_handle, RBPOWER_SOURCE_I2C_SENSOR,
                                                  2, i3_full[c].channel)) buf[n++] = i3_full[c];
        if (n > 0) rbgrid_config_add_rbamp_module(config_handle, 0x52, 2, buf, n);
        else ESP_LOGW(TAG, "rbAmp module 0x52: all channels unbound or already restored — no defaults created");
        }
    }
#endif

    /* ESP-NOW nodes: register from the PERSISTED registry (/espnow_nodes.bin) as STANDARD config
     * sensor-units → full registration at init (flash buffers, source mappings, accumulators), like a local
     * rbAmp. Paired/added nodes (espnow addnode verb / over-the-air pairing) live in the registry, which
     * persists. An empty registry stays empty — nodes join via `espnow pair` (over the air) or
     * `espnow addnode`. */
    {
        espnow_registry_t ereg;
        esp_now_registry_load(&ereg);
        if (ereg.node_count == 0)
            ESP_LOGW(TAG, "ESP-NOW registry empty → no nodes (use `espnow pair` to add over the air)");
        int total = 0;
        for (int nn = 0; nn < ereg.node_count; nn++) {
            const espnow_reg_node_t* nd = &ereg.nodes[nn];
            rbgrid_rbamp_channel_t chs[ESPNOW_REG_MAX_CH];
            for (int c = 0; c < nd->ch_count; c++) {
                rbsensor_type_t st;
                switch (nd->channels[c].ct_code) {
                    case 3: st = RBSENSOR_TYPE_CURRENT_SCT013_30A; break;
                    case 4: st = RBSENSOR_TYPE_CURRENT_SCT013_30A; break;  /* no 50A type → 30A */
                    case 6: st = RBSENSOR_TYPE_CURRENT_SCT013_20A; break;
                    default: st = RBSENSOR_TYPE_CURRENT_SCT013_10A; break; /* 1/2 → 10A */
                }
                chs[c].channel = nd->channels[c].ch;
                chs[c].sensor_type = st;
                chs[c].name = NULL;   /* auto-name */
            }
            rbgrid_config_add_espnow_module(config_handle, nd->mac, chs, nd->ch_count);
            total += nd->ch_count;
        }
        ESP_LOGW(TAG, "ESP-NOW nodes from registry: %d node(s), %d unit(s)", ereg.node_count, total);
    }
/*
  
    // Test loading units configuration from Flash
    ESP_LOGI(TAG, "=== Testing units configuration load from Flash ===");
    
    // First, let's check if units.json file exists
    if (littlefs_raw_exists("/units.json")) {
        ESP_LOGI(TAG, "Found /units.json file, attempting to load...");
        
        if (config_handle) {
            // Load units configuration from file
            esp_err_t load_ret = rbgrid_storage_load_units(config_handle);
            if (load_ret == ESP_OK) {
                ESP_LOGI(TAG, "✅ Units configuration loaded successfully from Flash");
            } else if (load_ret == ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "Units configuration file not found");
            } else {
                ESP_LOGE(TAG, "Failed to load units configuration: %s", esp_err_to_name(load_ret));
            }
        } else {
            ESP_LOGE(TAG, "Failed to create test config handle");
        }
    } else {
        ESP_LOGW(TAG, "No /units.json file found, skipping load test");
    }
    
    ESP_LOGI(TAG, "=== End of units configuration load test ===");
*/
 
    // Test loading tariff configuration from Flash
    ESP_LOGI(TAG, "=====================================");
    ESP_LOGI(TAG, "Testing tariff loading from Flash...");
    ESP_LOGI(TAG, "=====================================");
/*
    // Try to load tariff configuration from saved file
    esp_err_t load_err = rbgrid_storage_load_tariffs(config_handle);
    if (load_err == ESP_OK) {
        ESP_LOGI(TAG, "✅ Tariff configuration loaded from Flash successfully");
    } else if (load_err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "⚠️ No saved tariff configuration found in Flash");
        ESP_LOGI(TAG, "Please configure tariff via web interface and save it first");
    } else 
    {
        ESP_LOGE(TAG, "❌ Failed to load tariff configuration: %s", esp_err_to_name(load_err));
*/       
    // === TARIFF: load-or-default-and-save ===
    // If the configurator (web app) saved a plan, LOAD + apply it. Otherwise build the hardcoded
    // default plan AND persist it to /tariffs.json so it survives reboot + becomes the editable
    // base. (Same pattern as system-config above and /rbamp.json.) The hardcode is only the seed
    // until saved data exists.
    bool tariffs_loaded = false;
    if (littlefs_raw_exists("/tariffs.json")) {
        esp_err_t lt = rbgrid_storage_load_tariffs(config_handle);
        if (lt == ESP_OK) {
            ESP_LOGI(TAG, "✅ Tariff plan LOADED from flash (/tariffs.json)");
            tariffs_loaded = true;
        } else {
            ESP_LOGW(TAG, "Tariff load failed (%s) — building default plan", esp_err_to_name(lt));
        }
    }
    if (!tariffs_loaded) {
            ESP_LOGI(TAG, "=== Configuring DEFAULT tariff plan (no saved config) ===");
        
        // Create tariff builder for Germany
        rbgrid_tariff_builder_t tariff_builder = rbgrid_tariff_builder_create("EUR", "DE");
        if (!tariff_builder) {
            ESP_LOGE(TAG, "Failed to create tariff builder");
        } else {
            // Basic tariff zones (EUR/kWh)
            rbgrid_tariff_builder_add_time_zone(tariff_builder, "Night", 0.08f, "23:00-07:00");
            rbgrid_tariff_builder_add_time_zone(tariff_builder, "Day", 0.15f, "07:00-18:00");
            rbgrid_tariff_builder_add_time_zone(tariff_builder, "Peak", 0.25f, "18:00-23:00");
            
            // Weekend days (Saturday=0x40, Sunday=0x01)
            rbgrid_tariff_builder_add_weekday_zone(tariff_builder, "WEEKEND", 0.09f, 0x41);
            
            // German holidays
            //rbgrid_tariff_builder_set_country_holidays(tariff_builder, "DE", 0.085f);
            
            // Monthly fee and grid fee
            rbgrid_tariff_builder_set_monthly_fee(tariff_builder, 15.0f);
            rbgrid_tariff_builder_add_custom_fee(tariff_builder, "Grid Fee", 8.5f, RBPOWER_TARIFF_FIXED_MONTHLY);
            
            // Net metering for solar panels
            config_err = rbgrid_tariff_builder_set_net_metering(tariff_builder, 0.12f, 0.7f);
            if (config_err != RBGRID_CONFIG_OK) {
                ESP_LOGE(TAG, "Failed to set net metering: %d", config_err);
            } else {
                ESP_LOGI(TAG, "Net metering set: calculated rate mode, ratio 0.7");
            }

            // Apply tariff plan
            config_err = rbgrid_config_apply_tariff_plan(config_handle, tariff_builder);
            if (config_err != RBGRID_CONFIG_OK) {
                ESP_LOGE(TAG, "Failed to apply tariff plan: %d", config_err);
            } else {
                ESP_LOGI(TAG, "✅ Default tariff plan applied");
                // Persist the default so the NEXT boot LOADS it (and the web app edits from it).
                char* tjson = rbgrid_json_create_rbpower_tariff_plan_config(g_json_ctx);
                if (tjson) {
                    esp_err_t st = rbgrid_storage_save_tariffs(tjson);
                    ESP_LOGI(TAG, "Default tariff plan persisted to /tariffs.json: %s", esp_err_to_name(st));
                    free(tjson);
                } else {
                    ESP_LOGW(TAG, "Could not serialize default tariff plan for persist");
                }
            }

            // Clear builder
            rbgrid_tariff_builder_destroy(tariff_builder);
        }
    }   // end if (!tariffs_loaded) — default tariff built + persisted

    // Or configure individually (commented for example):
    // Basic telemetry settings
    rbgrid_config_set_telemetry_basic(config_handle, true, 4096, true);
    
    // Configure MQTT channel
    rbgrid_config_set_telemetry_mqtt(config_handle, false, 
                                     "mqtt://localhost:1883", 
                                     "rbpower/measurements", 
                                     1, 3000);
    
    // Configure HTTP channel  
    rbgrid_config_set_telemetry_http(config_handle, false,
                                     "https://api.example.com/rbpower/data",
                                     "your-api-token",
                                     15000);
    
    // Configure WebSocket channel
    rbgrid_config_set_telemetry_websocket(config_handle, true,
                                          "",   /* server URL: informational only (the hub itself serves WS) */
                                          8080,
                                          3000);

    
    // Additional extended_config settings through obtained pointer
    extended_config->rbhome_settings.logging_level = RBGRID_LOG_LEVEL_DEBUG;
    extended_config->enable_time_synchronization = true;
    
    // Configure auto-start measurements (default true)
    extended_config->auto_start_measurements = true;  // Automatically start measurements after initialization
    
    // Pass units array created through API functions
    extended_config->units_config.predefined_units = config_ctx->units_array;
    extended_config->units_config.predefined_units_count = config_ctx->units_count;
    extended_config->units_config.use_predefined_only = true;  // Use only predefined
    
    // Set global pointer
    g_extended_config = extended_config;
    size_t extflash_total = 0, extflash_used = 0;
    if (rbgrid_storage_get_stats(&extflash_total, &extflash_used) == ESP_OK) {
            ESP_LOGI(TAG, "Storage: %zu KB used of %zu KB total", extflash_used/1024, extflash_total/1024);
    }
//    esp_err_t HRW_ret = rbgrid_storage_save_hardware(config_ctx->hardware_config);

//    esp_err_t HRW_ret= rbgrid_storage_load_hardware(config_ctx->hardware_config);

    // ===== FLASH MODULE TESTS AND INITIALIZATION =====
    // IMPORTANT: must stay here, AFTER the units are configured and before rbgrid_init_system()
    if (rbpower_flash_is_available()) {
         ESP_LOGI(TAG, " ===  FLASH MODULES TESTS  ===");
 
        // Test 1: JSON configuration (old version)
        ESP_LOGI(TAG, "Test 1: Flash config (old version)...");
        rbpower_flash_config_init();
        rbpower_flash_config_test();
        ESP_LOGI(TAG, "");

        // Test 2: runtime configuration (optimized version)
        ESP_LOGI(TAG, "Test 2: Runtime config (optimized)...");
        rbpower_runtime_config_test();
        ESP_LOGI(TAG, "");

        // Test 3: load all JSON configs
        ESP_LOGI(TAG, "Test 3: Load all JSON configs...");
        rbpower_runtime_config_manager_t test_manager;
        if (rbpower_runtime_config_init(&test_manager) == ESP_OK) {
            esp_err_t load_ret = rbpower_runtime_config_load_all(&test_manager);
            if (load_ret == ESP_OK) {
                ESP_LOGI(TAG, "✓ load_all() succeeded:");
                rbpower_runtime_config_dump(&test_manager);
            } else if (load_ret == ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "No config files found (this is OK for first run)");
            } else {
                ESP_LOGE(TAG, "✗ load_all() failed: %s", esp_err_to_name(load_ret));
            }
            rbpower_runtime_config_deinit(&test_manager);
        }
        ESP_LOGI(TAG, "");

        // Test 4: flash operations (record write/read through sector markers)
        ESP_LOGI(TAG, "Test 4: Flash operations (Sector Markers)...");
        rbpower_flash_operations_init();
    } else {
        ESP_LOGW(TAG, "===  FLASH MODULES TESTS SKIPPED (Flash not available)  ===");
    }
   
    // Initialize rbgrid system (unit_UID mapping, NVS)

    ESP_LOGI("MAIN", "Initializing rbgrid system...");
    rbgrid_err_t rbgrid_err = rbgrid_init_system();
    if (rbgrid_err != RBGRID_OK) {
        ESP_LOGE("MAIN", "Failed to initialize rbgrid system: %d", rbgrid_err);
        ESP_LOGW("MAIN", "Unit UID mapping features may be limited");
    } else {
        ESP_LOGI("MAIN", "✅ rbgrid system initialized successfully");
    }
    
  
    // Initialize with home network
    rbpower_err_t power_err = rbpower_init_extended(extended_config);
    if (power_err != RBPOWER_OK) {
        ESP_LOGE("MAIN", "Failed to initialize extended rbpower: %d", power_err);
        ESP_LOGW("MAIN", "Continuing with limited functionality");
    }

    // Persist the DEFAULT sensor+unit config on FIRST boot (no saved files) so it reloads next boot
    // and the web app can edit from it. If config was LOADED from flash, do NOT re-save (preserve user
    // edits). g_home_network is fully built by now → save_units(NULL) serializes the live units.
    if (!have_saved_hw_units) {
        esp_err_t sh = rbgrid_storage_save_hardware(base_hardware_config);
        esp_err_t su = rbgrid_storage_save_units(NULL);
        ESP_LOGI("MAIN", "Default sensor+unit config persisted (hardware:%s units:%s)",
                 esp_err_to_name(sh), esp_err_to_name(su));
    } else {
        ESP_LOGI("MAIN", "Sensor+unit config was loaded from flash — not re-saving (preserve edits)");
    }

    // JSON context already initialized earlier
    if (extended_config->rbhome_JSON_settings.enable_json_api) {
        // Update JSON context if needed for production use
        ESP_LOGI(TAG, "JSON module already initialized, updating for production use");
        if (g_json_ctx) {
            // Update device info if needed
            g_json_ctx->device_info.device_id[0] = '\0';
            strlcpy(g_json_ctx->device_info.device_id, "rbgrid_main_001", sizeof(g_json_ctx->device_info.device_id));
            strlcpy(g_json_ctx->device_info.device_name, "MainPowerUnit", sizeof(g_json_ctx->device_info.device_name));
            strlcpy(g_json_ctx->device_info.location, "Main Panel", sizeof(g_json_ctx->device_info.location));
        }
/*
        // Save units configuration to Flash
        ESP_LOGI(TAG, "=== Saving units configuration to Flash ===");
        esp_err_t save_ret = rbgrid_storage_save_units(NULL);
        if (save_ret == ESP_OK) {
            ESP_LOGI(TAG, "✅ Units configuration saved successfully");
        } else {
            ESP_LOGE(TAG, "Failed to save units configuration: %s", esp_err_to_name(save_ret));
        }

*/        
        // start web server if WiFi is initialized
        if (wifi_initialized) {
            ESP_LOGI(TAG, "Starting web server");
            start_webserver();
            
            // Get IP for testing
            rbwifi_info_t wifi_info;
            if (rbwifi_get_status(&wifi_info) == RBPOWER_OK && wifi_info.status == RBWIFI_STATUS_CONNECTED) {
                ESP_LOGI(TAG, "Test at: http://" IPSTR "/system-config", IP2STR(&wifi_info.ip_info.ip));
            }
        }
    }
    
    // ===== TEST 5: Network structure =====
    rbpower_home_print_network_structure();

    // ===== TEST 6 (opt-in): external-source injection API self-test =====
    // Build with -DRBPOWER_RUN_INJECT_TESTS=1 to run on-target (a test script parses the
    // "RBTEST inject SUMMARY: <P> passed, <F> failed" UART line). 0x0004 is a unit
    // registered above by the demo config (same unit used by test_period_statistics_api).
#ifndef RBPOWER_RUN_INJECT_TESTS
/* 1 = run the injection self-test at boot (opt-in, default 0) */
#define RBPOWER_RUN_INJECT_TESTS 0  /* opt-in: build with -DRBPOWER_RUN_INJECT_TESTS=1 to run on-target */
#endif
#if RBPOWER_RUN_INJECT_TESTS
    {
        int inject_failures = rbpower_home_inject_run_tests((rbpower_unit_id_t)0x0004);
        ESP_LOGI("MAIN", "Injection self-test verdict: %s (%d failures)",
                 inject_failures == 0 ? "PASS" : "FAIL", inject_failures);
    }
#endif

    // ===== rbAmp I2C metering bridge bring-up =====
    // RBAMP_BRIDGE_ENABLE (default 1) probes + polls the rbAmp modules on I2C (default: mains UI1 @0x50).
    // NOTE: check the SDA/SCL pins of the rbAmp bus in the fleet config for your hardware.
    /* Start the rbAmp fleet as the source (RBGRID_RBAMP_FLEET — debug-bridge or wifi build). */
#if RBGRID_RBAMP_FLEET
    /* rbpower_stop() halts the built-in measurement engine, so the rbAmp injection is the SOLE feeder of units
     * 0x0006/0x0007 (the external-source path); rbAmp data shows via telemetry / `unit 0006`. */
    rbpower_stop();
    {
        /* Default config = single module @0x50 (voltage->0x0007, ch0 10A code2 ->0x0006). */
        rbamp_bridge_config_t rbamp_cfg = RBAMP_BRIDGE_DEFAULT_CONFIG();
        rbamp_cfg.inject_enabled = true;     // feed rbAmp readings into rbhome stats
        rbamp_cfg.poll_period_ms = 1000;     // 1s read. NOTE: the 3-sec period stats (what MQTT/
                                             // telemetry serialize) are built by the 200ms-cadence
                                             // accumulator, which SKIPS the first sample per setup
                                             // and needs several samples/period — at 3000ms there
                                             // was only 1 inject/period (skipped) → period stats 0.
                                             // 1000ms gives ~3 injects/period → non-zero. (~5x
                                             // fewer injects than a 200ms cadence.) Possible follow-on:
                                             // rbAmp 3-sec should write period stats directly
                                             // (device RMS) instead of SoS-accumulating, like the
                                             // 15-min inject_period_result path.
        rbamp_cfg.current_gain   = 1.0f;     // no host cal (per-unit gains in module flash)
#if RBGRID_DEFAULT_BENCH_FLEET
        rbamp_cfg.voltage_gain   = 0.974f;   // host trim for an uncalibrated development sample at 0x50
                                             // (~+2.6% U). Calibrated production modules want 1.0
                                             // (calgain reset). RBgrid never writes module cal.
        /* Default 3-module fleet: add I2(0x51) + I3(0x52). Their channels feed
         * the I2C-UID units created by rbgrid_config_add_rbamp_module() (config phase).
         * CT codes: 0x51 ch0=5A(1)/ch1=10A(2); 0x52 ch0=30A(3)/ch1=20A(6). */
        rbamp_cfg.module_count = 3;
        rbamp_cfg.modules[1].addr = 0x51; rbamp_cfg.modules[1].entry_count = 2;
        rbamp_cfg.modules[1].entries[0].source = 0; rbamp_cfg.modules[1].entries[0].ct_code = 1;
        rbamp_cfg.modules[1].entries[0].unit_id = (uint16_t)RBGRID_MAKE_I2C_UNIT_ID(0, 1, 0);
        rbamp_cfg.modules[1].entries[1].source = 1; rbamp_cfg.modules[1].entries[1].ct_code = 2;
        rbamp_cfg.modules[1].entries[1].unit_id = (uint16_t)RBGRID_MAKE_I2C_UNIT_ID(0, 1, 1);
        rbamp_cfg.modules[2].addr = 0x52; rbamp_cfg.modules[2].entry_count = 2;
        rbamp_cfg.modules[2].entries[0].source = 0; rbamp_cfg.modules[2].entries[0].ct_code = 3;
        rbamp_cfg.modules[2].entries[0].unit_id = (uint16_t)RBGRID_MAKE_I2C_UNIT_ID(0, 2, 0);
        rbamp_cfg.modules[2].entries[1].source = 1; rbamp_cfg.modules[2].entries[1].ct_code = 6;
        rbamp_cfg.modules[2].entries[1].unit_id = (uint16_t)RBGRID_MAKE_I2C_UNIT_ID(0, 2, 1);
#else
        rbamp_cfg.voltage_gain   = 1.0f;     // product default: factory-calibrated modules, no host trim
        /* product default fleet: one module at 0x50 (RBAMP_BRIDGE_DEFAULT_CONFIG); more modules are added by
         * the user (`rbampcfg`). */
#endif

        /* Config persistence: prefer the saved fleet config (/rbamp.json) if present; otherwise persist
         * THIS default so it survives reboot and can be edited later (the /api/v1/i2c API or the
         * `rbampcfg` verb). First boot saves the 3-module default fleet, subsequent boots load it. */
        rbamp_bridge_config_t rbamp_loaded;
        if (rbgrid_storage_load_rbamp(&rbamp_loaded) == ESP_OK) {
            rbamp_cfg = rbamp_loaded;
            ESP_LOGI("MAIN", "rbAmp fleet config LOADED from flash (%u modules)",
                     (unsigned)rbamp_cfg.module_count);
        } else {
            esp_err_t se = rbgrid_storage_save_rbamp(&rbamp_cfg);
            ESP_LOGI("MAIN", "rbAmp fleet config persisted (default fleet, %u module(s)): %s",
                     (unsigned)rbamp_cfg.module_count, esp_err_to_name(se));
        }

#if RBGRID_BENCH_KIT
        /* Test-rig variant (one ESP32 as hub and rig controller): load control + YP9830
         * reference meter live in THIS firmware via bench_kit. Must init BEFORE the rbAmp bus probe:
         * it raises the DUT straps (IO21=I2C transport, IO4=production/maintenance-off), releases
         * NRST and forces every relay bank OFF (PCF 0x27 powers up ALL-ON). Dead-man default
         * 7200s; the high-power coarse bank 0x24 stays blocked (allow_coarse=false). */
        bench_kit_config_t bk_cfg = BENCH_KIT_CONFIG_DEFAULT();
        esp_err_t bk_err = bench_kit_init(&bk_cfg);
        ESP_LOGI("MAIN", "bench_kit init: %s", esp_err_to_name(bk_err));
#endif

        esp_err_t rbamp_err = rbamp_bridge_start(&rbamp_cfg);
        ESP_LOGI("MAIN", "rbAmp bridge start: %s", esp_err_to_name(rbamp_err));
        /* Auto-collect rbAmp periods on the rbpower 15-min boundary (every build — energy statistics depend on it). */
        rbpower_register_period_boundary_cb(rbgrid_rbamp_period_cb);
        ESP_LOGI("MAIN", "rbAmp auto 15-min period collection armed");
    }
#endif

    /* Event log files (after rbpower storage init — it may own the `logs` mount) + boot forensics in EVERY
     * build: the classification feeds system.boot and the log; debug builds also echo it on the console. */
    rbevlog_attach_storage();
    static char s_boot_line[200];
    rbpower_boot_check(s_boot_line, sizeof(s_boot_line));
    rbpower_diag_printf("%s", s_boot_line);
    ota_report_rollback();

#if RBGRID_RBBENCH
    /* HW debug-bridge console (test harness). Quiet the logs so the
     * BRIDGE_SHELL line protocol stays clean, then start the console on the
     * shared sensor bus and register RBgrid introspection verbs. */
    esp_log_level_set("*", ESP_LOG_WARN);
    rbbench_init(rbamp_bridge_get_bus());
    rbbench_register_cmd("heap", rbgrid_dbg_heap);
    rbbench_register_cmd("shutdown", rbgrid_dbg_shutdown);
    rbbench_register_cmd("reboot", rbgrid_dbg_reboot);
    rbbench_register_cmd("wifidrop", rbgrid_dbg_wifidrop);
    rbbench_register_cmd("wifistat", rbgrid_dbg_wifistat);
    rbbench_register_cmd("phfeed", rbgrid_dbg_phfeed);
    rbbench_register_cmd("stdinfo", rbgrid_dbg_stdinfo);
    rbbench_register_cmd("unit", rbgrid_dbg_unit);
    rbbench_register_cmd("ctmodel", rbgrid_dbg_ctmodel);
    rbbench_register_cmd("ctget", rbgrid_dbg_ctget);
    rbbench_register_cmd("modreset", rbgrid_dbg_modreset);
    rbbench_register_cmd("ctsave", rbgrid_dbg_ctsave);
    rbbench_register_cmd("period", rbgrid_dbg_period);
    rbbench_register_cmd("pstat", rbgrid_dbg_pstat);
    rbbench_register_cmd("tariffdiag", rbgrid_dbg_tariffdiag);
    rbbench_register_cmd("tarifftime", rbgrid_dbg_tarifftime);
    rbbench_register_cmd("injtest", rbgrid_dbg_injtest);
    rbbench_register_cmd("inj200", rbgrid_dbg_inj200);
    rbbench_register_cmd("migdiag", rbgrid_dbg_migdiag);
    rbbench_register_cmd("statmeta", rbgrid_dbg_statmeta);
    rbbench_register_cmd("zonestats", rbgrid_dbg_zonestats);
    rbbench_register_cmd("netmeter", rbgrid_dbg_netmeter);
    rbbench_register_cmd("netsettle", rbgrid_dbg_netsettle);
    rbbench_register_cmd("lifetime", rbgrid_dbg_lifetime);     /* lifetime totalizers + NVS persist */
    rbbench_register_cmd("progmode", rbgrid_dbg_progmode);
    rbbench_register_cmd("progtier", rbgrid_dbg_progtier);
    rbbench_register_cmd("enbalance", rbgrid_dbg_enbalance);
    rbbench_register_cmd("sysflows", rbgrid_dbg_sysflows);
    rbbench_register_cmd("fmtinfo", rbgrid_dbg_fmtinfo);
    rbbench_register_cmd("teletime", rbgrid_dbg_teletime);
    rbbench_register_cmd("token", rbgrid_dbg_token);
    rbbench_register_cmd("webhttps", rbgrid_dbg_webhttps);
    rbbench_register_cmd("selftest", rbgrid_dbg_selftest);
    rbbench_register_cmd("calgain", rbgrid_dbg_calgain);
    rbbench_register_cmd("espnow", rbgrid_dbg_espnow);
    rbbench_register_cmd("status", rbgrid_dbg_status);
    rbbench_register_cmd("rbampcfg", rbgrid_dbg_rbampcfg);
    // External-flash stats test harness
    rbbench_register_cmd("flashinfo", rbgrid_dbg_flashinfo);
    rbbench_register_cmd("flashen", rbgrid_dbg_flashen);
    rbbench_register_cmd("forceperiod", rbgrid_dbg_forceperiod);
    rbbench_register_cmd("statforce1h", rbgrid_dbg_statforce1h);
    rbbench_register_cmd("rmunit", rbgrid_dbg_rmunit);
    rbbench_register_cmd("statread", rbgrid_dbg_statread);
    rbbench_register_cmd("statcursor", rbgrid_dbg_statcursor);
    rbbench_register_cmd("stattest", rbgrid_dbg_stattest);
    rbbench_register_cmd("flasherase", rbgrid_dbg_flasherase);
    rbbench_register_cmd("statfeed", rbgrid_dbg_statfeed);
    rbbench_register_cmd("statflow", rbgrid_dbg_statflow);
    rbbench_register_cmd("timerstat", rbgrid_dbg_timerstat);
    rbbench_register_cmd("modscan", rbgrid_dbg_modscan);
    rbbench_register_cmd("stdreg", rbgrid_dbg_stdreg);
    rbbench_register_cmd("basreg", rbgrid_dbg_basreg);
    rbbench_register_cmd("digest", rbgrid_dbg_digest);
    rbbench_register_cmd("libcheck", rbgrid_dbg_libcheck);
    rbbench_register_cmd("stacks", rbgrid_dbg_stacks);
    rbbench_register_cmd("appw", rbgrid_dbg_appw);
    rbbench_register_cmd("drdy", rbgrid_dbg_drdy);
    rbbench_register_cmd("crashtest", rbgrid_dbg_crashtest);
    rbbench_register_cmd("savecal", rbgrid_dbg_savecal);
    rbbench_register_cmd("etstat", rbgrid_dbg_etstat);
    rbbench_register_cmd("diag", rbgrid_dbg_diag);
    rbbench_register_cmd("vmod", rbgrid_dbg_vmod);
    rbbench_register_cmd("pollpause", rbgrid_dbg_pollpause);
    rbbench_register_cmd("leakprobe", rbgrid_dbg_leakprobe);
    rbbench_register_cmd("clock", rbgrid_dbg_clock);
    rbpower_diag_set_sink(rbgrid_diag_sink);          /* "#DIAG " trace on the debug console */
    rbpower_diag_set(true, 60);                        /* debug builds: on by default, 60 s cadence */
    {   /* boot forensics on the console + #DIAG (FAIL when the reset was a panic/watchdog/unplanned) */
        rbbench_reply("%s", s_boot_line); rbbench_reply("#DIAG %s", s_boot_line);
    }
    rbbench_register_cmd("gridstat", rbgrid_dbg_gridstat);
    rbbench_register_cmd("gridfeed", rbgrid_dbg_gridfeed);
#endif

    ESP_LOGI("MAIN", "=====Setup complete!=======");

    // After rbpower_flash_init_buffers_from_network()
 //       vTaskDelay(pdMS_TO_TICKS(5000));  // wait a few seconds for data to be written
/*
        ESP_LOGI("MAIN", "=== Testing Flash buffer reading ===");

        // Test 1: read the last 10 records of unit 0x0004 (single record read)
        ESP_LOGI("MAIN", "--- Test 1: Single record read (recent 10) ---");
        rbpower_flash_test_read_recent(0x0004, RBPOWER_PERIOD_HOUR, RBPOWER_DATA_TYPE_ELECTRICAL, 10);

        // Test 2: batch read of records 0-9 of unit 0x0004
        ESP_LOGI("MAIN", "--- Test 2: Batch read [0..9] ---");
        rbpower_flash_test_read_batch(0x0004, RBPOWER_PERIOD_HOUR, RBPOWER_DATA_TYPE_ELECTRICAL, 0, 10);

        // Test 3: batch read of records 10-19 of unit 0x0004 (older records)
        ESP_LOGI("MAIN", "--- Test 3: Batch read [10..19] ---");
        rbpower_flash_test_read_batch(0x0004, RBPOWER_PERIOD_HOUR, RBPOWER_DATA_TYPE_ELECTRICAL, 10, 10);

        // ═══════════════════════════════════════════════════════════════════════════════
        // PERIOD HISTORY API TEST: rbgrid_get_period_history()
        // ═══════════════════════════════════════════════════════════════════════════════
        ESP_LOGI("MAIN", "");
        ESP_LOGI("MAIN", "=== Running the period statistics API test ===");
        vTaskDelay(pdMS_TO_TICKS(1000)); // delay for log readability

        test_period_statistics_api();
*/
}

// ====== LOOP ======
/** Dead reference code (an old main-loop body, entirely inside while(0)).
 * Kept as documentation of the realtime read API; never called. */
__attribute__((unused)) static void demo_readout_loop(void) {
    // Main loop with periodic status output
    while (0) {
        
        // User code: get voltage statistics every 10 seconds
            statistics_3sec_final_t voltage_stats;
            rbgrid_err_t result = rbgrid_realtime_get_voltage(&voltage_stats);
            
            if (result == RBGRID_OK) {
                ESP_LOGI(TAG, "Voltage: %.2f V, Frequency: %.2f Hz, THD: %.2f%%",
                         voltage_stats.voltage_rms_final,
                         voltage_stats.frequency_avg_final,
                         voltage_stats.thd_voltage_avg_final);
            } else {
                ESP_LOGE(TAG, "Failed to get voltage statistics: %d", result);
            }
        
            
            // Get real-time Main Supply status (200ms data)
            float current_rms, power_active, power_reactive, power_apparent, power_factor, energy_increment;
            uint32_t samples_count;
            rbpower_current_direction_t direction;
            result = rbgrid_realtime_get_main_supply(&current_rms, &power_active, &direction,
                                                                      &power_reactive, &power_apparent, &power_factor,
                                                                      &samples_count, &energy_increment);
            
            if (result == RBGRID_OK) {
                const char* direction_str = rbpower_current_direction_to_string(direction);
                ESP_LOGI(TAG, "Main Supply Status: Current: %.3f A, Active: %.1f W, Reactive: %.1f VAR, Apparent: %.1f VA, PF: %.3f, Samples: %lu, Energy: %.3f Wh, Direction: %s",
                         current_rms, power_active, power_reactive, power_apparent, power_factor, samples_count, energy_increment, direction_str);
            } else {
                ESP_LOGE(TAG, "Failed to get main supply status: %d", result);
            }
            
            // Get real-time Alternative Supply status (200ms data)
            float alt_current_rms, alt_power_active, alt_power_reactive, alt_power_apparent, alt_power_factor, alt_energy_increment;
            float alt_efficiency, alt_dc_power;
            uint32_t alt_samples_count;
            rbpower_current_direction_t alt_direction;
            result = rbgrid_realtime_get_alt_supply(&alt_current_rms, &alt_power_active, &alt_direction,
                                                                         &alt_power_reactive, &alt_power_apparent, &alt_power_factor,
                                                                         &alt_samples_count, &alt_energy_increment,
                                                                         &alt_efficiency, &alt_dc_power);
            
            if (result == RBGRID_OK) {
                const char* alt_direction_str = rbpower_current_direction_to_string(alt_direction);
                ESP_LOGI(TAG, "Alt Supply Status: Current: %.3f A, Active: %.1f W, Reactive: %.1f VAR, Apparent: %.1f VA, PF: %.3f, Samples: %lu, Energy: %.3f Wh, Efficiency: %.1f%%, DC: %.1f W, Direction: %s",
                         alt_current_rms, alt_power_active, alt_power_reactive, alt_power_apparent, alt_power_factor, alt_samples_count, alt_energy_increment, alt_efficiency, alt_dc_power, alt_direction_str);
            } else {
                ESP_LOGE(TAG, "Failed to get alternative supply status: %d", result);
            }
            
            // Get 200ms data of regular unit (Heater) by unit_UID
            float unit_current, unit_power_active, unit_power_reactive, unit_power_apparent, unit_power_factor;
            uint32_t unit_samples;
            result = rbgrid_realtime_get_unit(heater_sensor_uid, &unit_current, 
                                                                    &unit_power_active, &unit_power_reactive, 
                                                                    &unit_power_apparent, &unit_power_factor, 
                                                                    &unit_samples);
            
            if (result == RBGRID_OK) {
                ESP_LOGI(TAG, "Heater Unit Status: I: %.3f A, P: %.1f W, Q: %.1f VAR, S: %.1f VA, PF: %.3f, Samples: %lu",
                         unit_current, unit_power_active, unit_power_reactive, unit_power_apparent, unit_power_factor, unit_samples);
            } else {
                ESP_LOGE(TAG, "Failed to get heater unit status: %d", result);
            }
            
            // Get 3-second economic data for Main Supply
            rbgrid_snapshot_base_t main_supply_economics;
            result = rbgrid_snapshot_get_main_supply(&main_supply_economics);
            
            if (result == RBGRID_OK) {
                const char* direction_str = rbpower_current_direction_to_string(main_supply_economics.direction);
                
                ESP_LOGI(TAG, "Main Supply Economics (3sec): Cost: %.4f, Revenue: %.4f, Rate: %.4f, Direction: %s, Consumed: %.3f Wh, Supplied: %.3f Wh",
                         main_supply_economics.cost_consumed, main_supply_economics.revenue_supplied, main_supply_economics.current_tariff_rate, 
                         direction_str, main_supply_economics.energy_consumed_wh, main_supply_economics.energy_supplied_wh);
            } else {
                ESP_LOGE(TAG, "Failed to get main supply economics: %d", result);
            }
            
            // Get 3-second economic data for Alternative Supply
            rbgrid_snapshot_base_t alt_supply_economics;
            result = rbgrid_snapshot_get_alt_supply(&alt_supply_economics);
            
            if (result == RBGRID_OK) {
                const char* alt_direction_str = rbpower_current_direction_to_string(alt_supply_economics.direction);
                
                ESP_LOGI(TAG, "Alt Supply Economics (3sec): GenCost: %.4f, GenRevenue: %.4f, Rate: %.4f, Direction: %s, Consumed: %.3f Wh, Used: %.3f Wh",
                         alt_supply_economics.cost_consumed, alt_supply_economics.revenue_supplied, 
                         alt_supply_economics.current_tariff_rate, 
                         alt_direction_str, alt_supply_economics.energy_consumed_wh, 
                         alt_supply_economics.energy_supplied_wh);
            } else {
                ESP_LOGE(TAG, "Failed to get alternative supply economics: %d", result);
            }
            
            // Get 3-second economic data for Heater Unit
            rbgrid_snapshot_base_t heater_unit_snapshot_base;
            result = rbgrid_snapshot_get_unit(heater_sensor_uid, &heater_unit_snapshot_base);

            if (result == RBGRID_OK) {
                ESP_LOGI(TAG, "Heater Unit Economics (3sec): CostConsumed: %.6f EUR, RevenueSupplied: %.6f EUR, NetCost: %.6f EUR, AvgTariff: %.4f EUR/kWh, Consumed: %.3f Wh, Supplied: %.3f Wh, NetBalance: %.3f Wh, Samples: %lu",
                         heater_unit_snapshot_base.cost_consumed, heater_unit_snapshot_base.revenue_supplied, (heater_unit_snapshot_base.cost_consumed - heater_unit_snapshot_base.revenue_supplied),
                         heater_unit_snapshot_base.avg_tariff_per_kwh, heater_unit_snapshot_base.energy_consumed_wh, heater_unit_snapshot_base.energy_supplied_wh,
                         (heater_unit_snapshot_base.energy_consumed_wh - heater_unit_snapshot_base.energy_supplied_wh), heater_unit_snapshot_base.samples_count);
            } else {
                ESP_LOGE(TAG, "Failed to get heater unit economics: %d", result);
            }

        vTaskDelay(pdMS_TO_TICKS(10000)); // 10 seconds
    }
    
    vTaskDelay(pdMS_TO_TICKS(1000));
}


