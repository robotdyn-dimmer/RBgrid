/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbwebserver_ota.c
 * @brief OTA firmware endpoints (part of the web server, see rbwebserver_internal.h).
 *
 * GET  /api/system/version — running firmware/partition description.
 * POST /api/system/ota     — stream a .bin into the next OTA slot, validate, set boot, reboot.
 *
 * Both handlers run on the httpd task.
 */

#include "rbwebserver_internal.h"
/** @brief Persist the open hour (PARTIAL record) before a planned restart (declared in rbhome_types.h). @return Number of units flushed. */
extern int rbpower_stats_flush_open_hours(void);   /* rbhome_types.h — open-hour flush on planned restart */
#include "rbstats_migrate.h"   /* storage_state in /api/system/version (deferred migration) */
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "esp_image_format.h"
#include "esp_rom_md5.h"
#include "mbedtls/sha256.h"       /* stream-hash the OTA upload */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "RBWEBSERVER_OTA";

/* Documented in rbwebserver.h. Computed once (~1 s of flash reads), then cached. */
const char* rbwebserver_running_image_md5(void)
{
    static char s_md5[33];
    static volatile bool s_done;
    if (s_done) return s_md5;
    char hex[33] = "";
    const esp_partition_t* run = esp_ota_get_running_partition();
    esp_image_metadata_t md;
    esp_partition_pos_t pos = { .offset = run ? run->address : 0, .size = run ? run->size : 0 };
    uint8_t* buf = run ? malloc(4096) : NULL;
    if (buf && esp_image_get_metadata(&pos, &md) == ESP_OK && md.image_len && md.image_len <= run->size) {
        md5_context_t ctx;
        esp_rom_md5_init(&ctx);
        bool ok = true;
        for (uint32_t off = 0; off < md.image_len && ok; off += 4096) {
            uint32_t n = md.image_len - off < 4096 ? md.image_len - off : 4096;
            ok = esp_partition_read(run, off, buf, n) == ESP_OK;
            if (ok) esp_rom_md5_update(&ctx, buf, n);
        }
        if (ok) {
            uint8_t d[16];
            esp_rom_md5_final(d, &ctx);
            for (int i = 0; i < 16; i++) sprintf(&hex[i * 2], "%02x", d[i]);
        }
    }
    free(buf);
    memcpy(s_md5, hex, sizeof(s_md5));
    s_done = true;
    return s_md5;
}

/* GET /api/system/version (documented in rbwebserver_internal.h). */
esp_err_t rbwebserver_api_version_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    const esp_app_desc_t* app = esp_app_get_description();
    const esp_partition_t* running = esp_ota_get_running_partition();
    char body[400];
    snprintf(body, sizeof(body),
             "{\"version\":\"%s\",\"project\":\"%s\",\"idf\":\"%s\",\"build_date\":\"%s %s\",\"partition\":\"%s\","
             "\"storage_state\":\"%s\",\"storage_progress\":%u,\"image_md5\":\"%s\"}",
             app ? app->version : "?", app ? app->project_name : "?", app ? app->idf_ver : "?",
             app ? app->date : "?", app ? app->time : "?", running ? running->label : "?",
             rbstats_store_state_str(), (unsigned)rbstats_migrate_progress(), rbwebserver_running_image_md5());
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

/* POST /api/system/ota — stream a firmware .bin into the OTA slot, validate, set boot, reboot.
 * Safe: esp_ota_end validates (magic/SHA) and the boot partition is only switched on success, so a
 * truncated/bad upload leaves the running image intact (factory stays as cable-free recovery). */
esp_err_t rbwebserver_api_ota_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    if (!rbapi_require_auth(req)) return ESP_FAIL;   // OTA flash requires the admin token (when provisioned)
    httpd_resp_set_type(req, "application/json");

    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* update = esp_ota_get_next_update_partition(NULL);
    bool to_factory = false;
    /* SECOND-OTA FIX: the table has factory + ONE ota slot, and
     * esp_ota_get_next_update_partition only iterates OTA-subtype slots — running from ota_0 it
     * returns ota_0 itself, esp_ota_begin then fails PARTITION_CONFLICT and every OTA after the
     * first one died with 500. Fall back to writing the FACTORY slot in that case (esp_ota_begin
     * accepts any non-running app partition). Trade-off until the table gains ota_1 (a partition
     * table change needs a full reflash): a factory-targeted update has NO auto-rollback (otadata is
     * erased to boot factory), which the response flags as rollback:"none". */
    if (!update || update == running) {
        const esp_partition_t* factory = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
        if (factory && factory != running) {
            update = factory;
            to_factory = true;
        } else {
            update = NULL;
        }
    }
    if (!update) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"no usable update partition\"}");
        return ESP_OK;
    }
    if (req->content_len <= 0 || (size_t)req->content_len > update->size) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"missing or oversized firmware\"}");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "OTA: receiving %d bytes into '%s' (%lu KB)", req->content_len, update->label,
             (unsigned long)(update->size / 1024));

    /* Optional integrity pins from the uploader — verified BEFORE activation.
     * X-FW-SHA256: 64-hex digest of the .bin; X-FW-Version: expected esp_app_desc version string.
     * Absent headers keep the legacy flow (esp_ota_end's own magic+SHA image validation only). */
    char want_sha[72] = {0}, want_ver[36] = {0};
    httpd_req_get_hdr_value_str(req, "X-FW-SHA256", want_sha, sizeof(want_sha));
    httpd_req_get_hdr_value_str(req, "X-FW-Version", want_ver, sizeof(want_ver));
    if (want_sha[0] && strlen(want_sha) != 64) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"X-FW-SHA256 must be 64 hex chars\"}");
        return ESP_OK;
    }

    /* Factory-target path: esp_ota_begin REFUSES non-OTA subtypes (is_ota_partition), so the
     * factory slot is written with the raw partition API. Image validity is still enforced:
     * X-FW-SHA256 is REQUIRED here, and esp_ota_set_boot_partition() runs the image validation
     * before switching (returns OTA_VALIDATE_FAILED on a bad image, boot selector untouched). */
    esp_ota_handle_t ota = 0;
    if (to_factory) {
        if (!want_sha[0]) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"factory-target OTA requires X-FW-SHA256\"}");
            return ESP_OK;
        }
        /* Erase happens ON THE FLY inside the write loop (erase-ahead per 64 KB, interleaved
         * with recv) — the same cadence OTA_WITH_SEQUENTIAL_WRITES uses. Up-front bulk erase
         * (full or 64 KB-chunked) stalls the CPU long enough that the upload socket resets
         * (observed in the field). Nothing to do here. */
    } else if (esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"ota_begin failed\"}");
        return ESP_OK;
    }
    char* buf = malloc(4096);
    if (!buf) { if (!to_factory) esp_ota_abort(ota); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0 /* SHA-256 */);
    int remaining = req->content_len;
    size_t woff = 0, f_erased = 0;
    bool ok = true;
    int stalls = 0;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining > 4096 ? 4096 : remaining);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++stalls <= 3) continue;   /* bounded: a stall pinned httpd forever */
        stalls = 0;
        if (r <= 0) { ok = false; break; }
        if (to_factory) {
            /* erase-ahead: make sure [woff, woff+r) is erased, one 64 KB block at a time */
            while (f_erased < woff + (size_t)r) {
                size_t chunk = 65536;
                if (f_erased + chunk > update->size) chunk = update->size - f_erased;
                if (chunk == 0 || esp_partition_erase_range(update, f_erased, chunk) != ESP_OK) { ok = false; break; }
                f_erased += chunk;
                vTaskDelay(1);
            }
            if (!ok) break;
            if (esp_partition_write(update, woff, buf, r) != ESP_OK) { ok = false; break; }
        } else if (esp_ota_write(ota, buf, r) != ESP_OK) { ok = false; break; }
        woff += (size_t)r;
        mbedtls_sha256_update(&sha, (const unsigned char*)buf, r);
        remaining -= r;
    }
    free(buf);
    unsigned char digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    if (!ok) {
        if (!to_factory) esp_ota_abort(ota);
        rbpower_diag_printf("FAIL ota: upload into '%s' broke at %u of %d bytes (running image intact)",
                            update->label, (unsigned)woff, req->content_len);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"recv/write failed\"}");
        return ESP_OK;
    }
    if (want_sha[0]) {
        char got_hex[65];
        for (int i = 0; i < 32; i++) sprintf(&got_hex[i * 2], "%02x", digest[i]);
        got_hex[64] = '\0';
        if (strcasecmp(got_hex, want_sha) != 0) {
            if (!to_factory) esp_ota_abort(ota);
            ESP_LOGE(TAG, "OTA REJECTED: sha256 mismatch (got %s)", got_hex);
            rbpower_diag_printf("FAIL ota: sha256 mismatch — image rejected (got %.16s…)", got_hex);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"sha256 mismatch — image rejected\"}");
            return ESP_OK;
        }
    }
    if (!to_factory) {
        esp_err_t err = esp_ota_end(ota);   // validates image (magic + SHA256)
        if (err != ESP_OK) {
            rbpower_diag_printf("FAIL ota: invalid image (ota_end %s)", esp_err_to_name(err));
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"invalid image (ota_end)\"}");
            return ESP_OK;
        }
    }
    if (want_ver[0]) {
        esp_app_desc_t desc;
        if (esp_ota_get_partition_description(update, &desc) != ESP_OK ||
            strncmp(desc.version, want_ver, sizeof(desc.version)) != 0) {
            ESP_LOGE(TAG, "OTA REJECTED: version mismatch (image '%s' != expected '%s')",
                     desc.version, want_ver);
            rbpower_diag_printf("FAIL ota: version mismatch (image '%.31s' != expected '%.31s')", desc.version, want_ver);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"version mismatch — image rejected\"}");
            return ESP_OK;   /* boot partition NOT switched — running image intact */
        }
    }
    esp_err_t sb = esp_ota_set_boot_partition(update);   /* factory: validates image + erases otadata */
    if (sb != ESP_OK) {
        ESP_LOGE(TAG, "set_boot failed: %s", esp_err_to_name(sb));
        rbpower_diag_printf("FAIL ota: set_boot '%s' failed: %s", update->label, esp_err_to_name(sb));
        httpd_resp_set_status(req, "500 Internal Server Error");
        char eb[96];
        snprintf(eb, sizeof(eb), "{\"status\":\"error\",\"message\":\"set_boot failed: %s\"}", esp_err_to_name(sb));
        httpd_resp_sendstr(req, eb);
        return ESP_OK;
    }
    char body[192];
    snprintf(body, sizeof(body),
             "{\"status\":\"ok\",\"message\":\"flashed, rebooting\",\"partition\":\"%s\",\"rollback\":\"%s\"}",
             update->label, to_factory ? "none" : "auto");
    httpd_resp_sendstr(req, body);
    ESP_LOGW(TAG, "OTA applied to '%s' — rebooting in 1s", update->label);
    {
        esp_app_desc_t nd;
        bool hv = esp_ota_get_partition_description(update, &nd) == ESP_OK;
        rbpower_diag_printf("ota: %d bytes into '%s' (from '%s'), version '%.31s', rollback=%s — rebooting",
                            req->content_len, update->label, running ? running->label : "?",
                            hv ? nd.version : "?", to_factory ? "none" : "auto");
    }
    vTaskDelay(pdMS_TO_TICKS(1000));   // let the response flush
    /* Planned restart: persist the open hour (PARTIAL record) before going down — field OTA is
     * the most common reboot, and it used to drop up to an hour of aggregated quarters. */
    (void)rbpower_stats_flush_open_hours();
    /* Mark it PLANNED (runs the pre-restart hook too): without the marker the next boot classified every
     * OTA as "software reset (NOT planned: panic/abort?)" — a false FAIL on OTA-only hubs. */
    rbpower_mark_planned_restart();
    esp_restart();
    return ESP_OK;
}
