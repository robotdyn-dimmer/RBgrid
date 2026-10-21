/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbwebserver_ui.c
 * @brief Embedded web-UI hosting: serves an uploaded web-app build and one language pack from flash.
 *
 * The 5 MB `storage` partition (littlefs, mounted at /storage by the storage layer) holds the web-app build:
 *   /storage/ui/cur/   — the live UI (served)
 *   /storage/ui/next/  — upload staging
 *   /storage/ui/lang/  — the optional language pack
 * Upload protocol:
 *   POST /api/system/ui-upload?path=\<stored\>   raw body (admin token) → /storage/ui/next/\<stored\>
 *   POST /api/system/ui-commit                 body = ui-manifest.json → sha256+size verify of
 *       every staged file → atomic swap (cur→old, next→cur, rm old) → manifest kept in cur/.
 * Serving rule: request P -> try P.gz (Content-Encoding: gzip, Content-Type by P's
 * extension) → else P. Hashed static assets = immutable cache; index.html = no-cache; rest 1 h.
 * No UI installed → the firmware stub (rbwebserver_sta_home_handler) keeps working; a failed
 * commit never breaks the running UI. All handlers run on the httpd task.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_littlefs.h"
#include "esp_http_server.h"
#include "mbedtls/sha256.h"
#include "cJSON.h"
#include "rbwebserver_internal.h"

static const char *TAG = "RBWEB_UI";

/* The UI lives under the `storage` partition. That partition is mounted at /storage by the
 * storage layer (rbstorage) — registering a SECOND littlefs on it returns INVALID_STATE, and
 * winning the boot race against rbstorage would push its recovery into a reformat. So the UI
 * NEVER registers: it reuses /storage (lazy retry covers boot ordering). */
#define UIFS_MOUNT  "/storage"          ///< VFS mount point of the storage partition (owned by rbstorage)
#define UIFS_BASE   UIFS_MOUNT "/ui"   ///< Root of the UI tree
#define UIFS_CUR    UIFS_BASE "/cur"   ///< Live UI (served)
#define UIFS_NEXT   UIFS_BASE "/next"  ///< Upload staging
#define UIFS_OLD    UIFS_BASE "/old"   ///< Previous UI during the commit swap
#define UI_MAX_PATH 160                ///< Maximum full path length, bytes
#define UI_CHUNK    4096               ///< Read/write chunk size, bytes

static bool s_mounted = false;
static esp_err_t s_mount_err = ESP_OK;   /* last mount failure, surfaced in API errors */
static bool s_installed = false;   /* cur/index.html(.gz) exists — cached, refreshed on commit */

/* ---------------------------------------------------------------- helpers */

static bool path_component_ok(const char *p)
{
    /* relative, no "..", conservative charset */
    if (!p || !p[0] || p[0] == '/' || strstr(p, "..")) return false;
    if (strlen(p) > 120) return false;
    for (const char *c = p; *c; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
              *c == '.' || *c == '_' || *c == '-' || *c == '/' || *c == '+'))
            return false;
    }
    return true;
}

static void mkdirs_for(const char *full_path)
{
    char tmp[UI_MAX_PATH];
    strlcpy(tmp, full_path, sizeof(tmp));
    for (char *c = tmp + strlen(UIFS_BASE) + 1; *c; c++) {
        if (*c == '/') { *c = '\0'; mkdir(tmp, 0755); *c = '/'; }
    }
}

static void remove_tree(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char p[UI_MAX_PATH + 256];
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(p);
        else unlink(p);
    }
    closedir(d);
    rmdir(dir);
}

static bool file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && !S_ISDIR(st.st_mode);
}

static void refresh_installed(void)
{
    s_installed = file_exists(UIFS_CUR "/index.html.gz") || file_exists(UIFS_CUR "/index.html");
}

static const char *content_type_for(const char *path /* WITHOUT .gz */)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html";
    if (!strcmp(dot, ".js"))   return "application/javascript";
    if (!strcmp(dot, ".css"))  return "text/css";
    if (!strcmp(dot, ".json")) return "application/json";
    if (!strcmp(dot, ".woff2"))return "font/woff2";
    if (!strcmp(dot, ".png"))  return "image/png";
    if (!strcmp(dot, ".svg"))  return "image/svg+xml";
    if (!strcmp(dot, ".ico"))  return "image/x-icon";
    if (!strcmp(dot, ".map"))  return "application/json";
    if (!strcmp(dot, ".txt"))  return "text/plain";
    return "application/octet-stream";
}

/* sha256 of a file, hex lowercase into out[65]. false on IO error. */
static bool sha256_file(const char *path, char out[65])
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    unsigned char *buf = malloc(UI_CHUNK);
    if (!buf) { fclose(f); return false; }
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    size_t n;
    while ((n = fread(buf, 1, UI_CHUNK, f)) > 0) mbedtls_sha256_update(&ctx, buf, n);
    unsigned char dig[32];
    mbedtls_sha256_finish(&ctx, dig);
    mbedtls_sha256_free(&ctx);
    free(buf);
    fclose(f);
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", dig[i]);
    out[64] = '\0';
    return true;
}

/* ---------------------------------------------------------------- mount */

esp_err_t rbwebserver_ui_mount(void)
{
    if (s_mounted) return ESP_OK;
    /* Reuse the rbstorage mount — just check it is up. */
    DIR *d = opendir(UIFS_MOUNT);
    if (!d) {
        s_mount_err = ESP_ERR_INVALID_STATE;   /* rbstorage not mounted (yet) — lazy retry later */
        ESP_LOGW(TAG, "%s not available yet (rbstorage mount pending?) — UI hosting deferred", UIFS_MOUNT);
        return ESP_ERR_INVALID_STATE;
    }
    closedir(d);
    s_mount_err = ESP_OK;
    s_mounted = true;
    mkdir(UIFS_BASE, 0755);
    mkdir(UIFS_CUR, 0755);
    mkdir(UIFS_NEXT, 0755);
    remove_tree(UIFS_OLD);                /* crash mid-swap leftover */
    refresh_installed();
    size_t total = 0, used = 0;
    esp_littlefs_info("storage", &total, &used);
    ESP_LOGI(TAG, "UI storage mounted: %u/%u KB used, ui %s",
             (unsigned)(used / 1024), (unsigned)(total / 1024),
             s_installed ? "INSTALLED" : "not installed (stub active)");
    return ESP_OK;
}

bool rbwebserver_ui_installed(void)
{
    if (!s_mounted) rbwebserver_ui_mount();   /* lazy heal: webserver may start before rbstorage */
    return s_mounted && s_installed;
}

/* ---------------------------------------------------------------- serving */

/* Serve /uifs/cur/<rel> honoring the P.gz-then-P rule. rel is sanitized by the caller. */
static esp_err_t serve_rel(httpd_req_t *req, const char *rel)
{
    char pgz[UI_MAX_PATH + 40], p[UI_MAX_PATH + 32];
    snprintf(p, sizeof(p), UIFS_CUR "/%s", rel);
    snprintf(pgz, sizeof(pgz), "%s.gz", p);
    const char *fpath = NULL;
    bool gz = false;
    if (file_exists(pgz)) { fpath = pgz; gz = true; }
    else if (file_exists(p)) { fpath = p; }
    else return ESP_ERR_NOT_FOUND;

    httpd_resp_set_type(req, content_type_for(p));
    if (gz) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    if (strncmp(rel, "static/", 7) == 0) {
        httpd_resp_set_hdr(req, "Cache-Control", "max-age=31536000, immutable");
    } else if (strcmp(rel, "index.html") == 0) {
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    } else {
        httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    }

    FILE *f = fopen(fpath, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    char *buf = malloc(UI_CHUNK);
    if (!buf) { fclose(f); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    size_t n;
    while ((n = fread(buf, 1, UI_CHUNK, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) break;
    }
    free(buf);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

esp_err_t rbwebserver_ui_index_handler(httpd_req_t *req)
{
    return serve_rel(req, "index.html");
}

/* ---------------------------------------------------------------- language pack
 * One device = one user = one language. English is built into the app; ONE alternative pack (JSON, ~110 KB)
 * may live on the hub so an app served by the hub needs no catalog. Stored next to the UI kit but outside
 * cur/next/old, so a UI commit never removes it. A new pack replaces the previous one. */
#define LANG_DIR      UIFS_BASE "/lang"   ///< Directory of the stored language pack
#define LANG_MAX      (200 * 1024)        ///< Maximum language-pack size, bytes

static bool lang_code_ok(const char *c)
{
    return c && isalpha((unsigned char)c[0]) && isalpha((unsigned char)c[1]) && c[2] == '\0';
}

/* the stored pack's code (lower-case) into out[3]; false when none */
static bool lang_current(char out[3])
{
    DIR *d = opendir(LANG_DIR);
    if (!d) return false;
    bool found = false;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *n = de->d_name;
        if (strlen(n) == 7 && !strcmp(n + 2, ".json") && isalpha((unsigned char)n[0]) && isalpha((unsigned char)n[1])) {
            out[0] = n[0]; out[1] = n[1]; out[2] = '\0';
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

/* GET /lang/<xx>.json — the stored pack as static JSON (404 when absent) */
static esp_err_t serve_lang(httpd_req_t *req, const char *rel /* "lang/xx.json" */)
{
    char p[UI_MAX_PATH + 32];
    snprintf(p, sizeof(p), UIFS_BASE "/%s", rel);
    FILE *f = fopen(p, "rb");
    if (!f) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "language pack not on this hub"); return ESP_FAIL; }
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");   /* the pack can be replaced under the same name */
    char *buf = malloc(UI_CHUNK);
    if (!buf) { fclose(f); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    size_t n;
    while ((n = fread(buf, 1, UI_CHUNK, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) break;
    }
    free(buf);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* GET  /api/system/lang            → {"code":"de","bytes":N} | {"code":null}
 * POST /api/system/lang?code=xx    body = the pack JSON (admin token) → stored, the previous pack removed */
esp_err_t rbwebserver_ui_lang_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    if (req->method == HTTP_OPTIONS) { httpd_resp_set_status(req, "204 No Content"); return httpd_resp_send(req, NULL, 0); }
    if (!s_mounted) rbwebserver_ui_mount();
    httpd_resp_set_type(req, "application/json");
    if (req->method == HTTP_GET) {
        char cur[3], p[64], out[80];
        if (s_mounted && lang_current(cur)) {
            struct stat st; snprintf(p, sizeof(p), LANG_DIR "/%s.json", cur);
            long sz = (stat(p, &st) == 0) ? (long)st.st_size : 0;
            snprintf(out, sizeof(out), "{\"code\":\"%s\",\"bytes\":%ld,\"max_bytes\":%d}", cur, sz, LANG_MAX);
        } else {
            snprintf(out, sizeof(out), "{\"code\":null,\"max_bytes\":%d}", LANG_MAX);
        }
        return httpd_resp_sendstr(req, out);
    }
    if (!rbapi_require_auth(req)) return ESP_FAIL;
    if (!s_mounted) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "UI storage not mounted"); return ESP_FAIL; }
    char query[64] = {0}, code[8] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "code", code, sizeof(code)) != ESP_OK || !lang_code_ok(code)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "?code= must be an ISO 639-1 code (2 letters)");
        return ESP_FAIL;
    }
    code[0] = (char)tolower((unsigned char)code[0]); code[1] = (char)tolower((unsigned char)code[1]);
    if (req->content_len <= 0 || req->content_len > LANG_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "pack missing or larger than 200 KB");
        return ESP_FAIL;
    }
    mkdir(UIFS_BASE, 0775);
    mkdir(LANG_DIR, 0775);
    const char *tmp = LANG_DIR "/.incoming";
    FILE *f = fopen(tmp, "wb");
    if (!f) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed"); return ESP_FAIL; }
    char *buf = malloc(UI_CHUNK);
    if (!buf) { fclose(f); remove(tmp); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    int remaining = req->content_len, stalls = 0;
    bool first = true, ok = true;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining > UI_CHUNK ? UI_CHUNK : remaining);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++stalls <= 3) continue;
        if (r <= 0) { ok = false; break; }
        stalls = 0;
        if (first) {   /* cheap sanity: a JSON object, not some other file */
            int k = 0; while (k < r && (buf[k] == ' ' || buf[k] == '\n' || buf[k] == '\r' || buf[k] == '\t')) k++;
            if (k < r && buf[k] != '{') { ok = false; break; }
            first = false;
        }
        if (fwrite(buf, 1, r, f) != (size_t)r) { ok = false; break; }
        remaining -= r;
    }
    free(buf);
    fclose(f);
    if (!ok || remaining != 0) {
        remove(tmp);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "upload broke or the body is not a JSON object");
        return ESP_FAIL;
    }
    char cur[3], p[64];
    if (lang_current(cur)) { snprintf(p, sizeof(p), LANG_DIR "/%s.json", cur); remove(p); }   /* at most one pack */
    snprintf(p, sizeof(p), LANG_DIR "/%s.json", code);
    if (rename(tmp, p) != 0) { remove(tmp); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "store failed"); return ESP_FAIL; }
    char out[96];
    snprintf(out, sizeof(out), "{\"status\":\"ok\",\"code\":\"%s\",\"bytes\":%d}", code, req->content_len);
    ESP_LOGI(TAG, "language pack '%s' stored (%d B)", code, req->content_len);
    return httpd_resp_sendstr(req, out);
}

/* GET catch-all (registered LAST — every /api, /wifi, /ws handler wins by registration order). */
esp_err_t rbwebserver_ui_file_handler(httpd_req_t *req)
{
    /* In AP (provisioning) mode keep the captive behavior: unknown paths go to /wifi. */
    if (is_request_from_ap_network(req)) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/wifi");
        return httpd_resp_send(req, NULL, 0);
    }
    char rel[UI_MAX_PATH];
    const char *uri = req->uri;
    size_t len = strcspn(uri, "?");                 /* strip query */
    if (len == 13 && !strncmp(uri, "/lang/", 6) && !strncmp(uri + 8, ".json", 5) &&
        isalpha((unsigned char)uri[6]) && isalpha((unsigned char)uri[7])) {   /* /lang/xx.json — served without a UI kit too */
        if (!s_mounted) rbwebserver_ui_mount();
        char lr[16]; snprintf(lr, sizeof(lr), "lang/%c%c.json", tolower((unsigned char)uri[6]), tolower((unsigned char)uri[7]));
        return serve_lang(req, lr);
    }
    if (!rbwebserver_ui_installed()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "UI not installed");
        return ESP_FAIL;
    }
    if (len <= 1) return serve_rel(req, "index.html");
    if (len >= sizeof(rel)) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "path"); return ESP_FAIL; }
    memcpy(rel, uri + 1, len - 1);                  /* drop leading '/' */
    rel[len - 1] = '\0';
    if (!path_component_ok(rel)) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "path"); return ESP_FAIL; }
    if (serve_rel(req, rel) == ESP_OK) return ESP_OK;
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_FAIL;
}

/* ---------------------------------------------------------------- upload / commit */

esp_err_t rbwebserver_ui_upload_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    if (!rbapi_require_auth(req)) return ESP_FAIL;
    if (!s_mounted) rbwebserver_ui_mount();   /* lazy heal: boot-time mount may fail transiently */
    if (!s_mounted) {
        char e[96];
        snprintf(e, sizeof(e), "UI storage not mounted (%s)", esp_err_to_name(s_mount_err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, e);
        return ESP_FAIL;
    }

    char query[192] = {0}, rel[144] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", rel, sizeof(rel)) != ESP_OK ||
        !path_component_ok(rel)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad or missing ?path=");
        return ESP_FAIL;
    }
    char full[UI_MAX_PATH + 32];
    snprintf(full, sizeof(full), UIFS_NEXT "/%s", rel);
    mkdirs_for(full);
    FILE *f = fopen(full, "wb");
    if (!f) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed"); return ESP_FAIL; }

    char *buf = malloc(UI_CHUNK);
    if (!buf) { fclose(f); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    int remaining = req->content_len;
    size_t written = 0;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining > UI_CHUNK ? UI_CHUNK : remaining);
        if (r <= 0) break;
        if (fwrite(buf, 1, r, f) != (size_t)r) { remaining = -1; break; }
        written += r;
        remaining -= r;
    }
    free(buf);
    fclose(f);
    if (remaining != 0) {
        unlink(full);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"status\":\"OK\",\"path\":\"%.64s\",\"size\":%u}", rel, (unsigned)written);
    httpd_resp_sendstr(req, resp);
    ESP_LOGI(TAG, "ui-upload %s (%u B)", rel, (unsigned)written);
    return ESP_OK;
}

/* Remove every file under `dir` whose path relative to UIFS_NEXT is not a manifest entry
 * (stored, or path as the fallback the verifier also accepts). Empty subdirs are left behind —
 * harmless, they are swept with the directory swap. */
static bool manifest_has_rel(cJSON *files, const char *rel)
{
    cJSON *fe;
    cJSON_ArrayForEach(fe, files) {
        cJSON *j = cJSON_GetObjectItem(fe, "stored");
        if (!cJSON_IsString(j)) j = cJSON_GetObjectItem(fe, "path");
        if (cJSON_IsString(j) && strcmp(j->valuestring, rel) == 0) return true;
    }
    return false;
}
static void prune_dir_to_manifest(const char *dir, cJSON *files)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char p[UI_MAX_PATH + 256];
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { prune_dir_to_manifest(p, files); continue; }
        const char *rel = p + strlen(UIFS_NEXT) + 1;
        if (!manifest_has_rel(files, rel)) {
            ESP_LOGW(TAG, "ui-commit: pruning stray staged file %s", rel);
            unlink(p);
        }
    }
    closedir(d);
}

esp_err_t rbwebserver_ui_commit_handler(httpd_req_t *req)
{
    add_cors_headers(req);
    if (!rbapi_require_auth(req)) return ESP_FAIL;
    if (!s_mounted) rbwebserver_ui_mount();
    if (!s_mounted) {
        char e[96];
        snprintf(e, sizeof(e), "UI storage not mounted (%s)", esp_err_to_name(s_mount_err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, e);
        return ESP_FAIL;
    }
    if (req->content_len <= 0 || req->content_len > 16384) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "manifest size");
        return ESP_FAIL;
    }
    char *body = malloc(req->content_len + 1);
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) { free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv"); return ESP_FAIL; }
        got += r;
    }
    body[got] = '\0';

    cJSON *m = cJSON_Parse(body);
    if (!m) { free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "manifest JSON"); return ESP_FAIL; }

    /* api_version guard: the UI must target this firmware's API (force=1 overrides). */
    cJSON *av = cJSON_GetObjectItem(m, "api_version");
    bool force = false;
    {
        char q[64] = {0}, v[8] = {0};
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
            httpd_query_key_value(q, "force", v, sizeof(v)) == ESP_OK && v[0] == '1') force = true;
    }
    if (!force && (!cJSON_IsNumber(av) || (int)av->valuedouble != RBGRID_API_VERSION)) {
        char err[128];
        snprintf(err, sizeof(err),
                 "{\"error\":\"api_version_mismatch\",\"fw\":%d,\"ui\":%d}",
                 RBGRID_API_VERSION, cJSON_IsNumber(av) ? (int)av->valuedouble : -1);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, err);
        cJSON_Delete(m);
        free(body);
        return ESP_FAIL;
    }

    /* verify every file: size + sha256 of the STORED bytes */
    cJSON *files = cJSON_GetObjectItem(m, "files");
    int verified = 0;
    const char *fail = NULL;
    char fail_path[144] = {0};
    cJSON *fe;
    cJSON_ArrayForEach(fe, files) {
        cJSON *jstored = cJSON_GetObjectItem(fe, "stored");
        if (!cJSON_IsString(jstored)) jstored = cJSON_GetObjectItem(fe, "path");
        cJSON *jsha = cJSON_GetObjectItem(fe, "sha256");
        cJSON *jsize = cJSON_GetObjectItem(fe, "size");
        if (!cJSON_IsString(jstored) || !cJSON_IsString(jsha) || !cJSON_IsNumber(jsize) ||
            !path_component_ok(jstored->valuestring)) { fail = "manifest entry"; break; }
        char full[UI_MAX_PATH + 32];
        snprintf(full, sizeof(full), UIFS_NEXT "/%s", jstored->valuestring);
        struct stat st;
        if (stat(full, &st) != 0 || st.st_size != (long)jsize->valuedouble) {
            strlcpy(fail_path, jstored->valuestring, sizeof(fail_path));
            fail = "size mismatch / missing";
            break;
        }
        char hex[65];
        if (!sha256_file(full, hex) || strcasecmp(hex, jsha->valuestring) != 0) {
            strlcpy(fail_path, jstored->valuestring, sizeof(fail_path));
            fail = "sha256 mismatch";
            break;
        }
        verified++;
    }
    if (!fail && verified == 0) fail = "empty manifest";
    if (fail) {
        char err[224];
        snprintf(err, sizeof(err), "{\"error\":\"verify_failed\",\"reason\":\"%s\",\"file\":\"%.100s\"}",
                 fail, fail_path);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, err);
        cJSON_Delete(m);
        free(body);
        return ESP_FAIL;
    }
    /* prune staging: files NOT in the manifest must not be promoted (a stray upload from an
     * aborted earlier session would otherwise ride along). */
    prune_dir_to_manifest(UIFS_NEXT, files);
    cJSON_Delete(m);

    /* atomic swap: cur→old, next→cur, drop old, fresh staging */
    remove_tree(UIFS_OLD);
    struct stat st;
    if (stat(UIFS_CUR, &st) == 0) rename(UIFS_CUR, UIFS_OLD);
    if (rename(UIFS_NEXT, UIFS_CUR) != 0) {
        rename(UIFS_OLD, UIFS_CUR);   /* roll back */
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "swap failed");
        return ESP_FAIL;
    }
    remove_tree(UIFS_OLD);
    mkdir(UIFS_NEXT, 0755);

    /* keep the manifest next to the files (also served as /ui-manifest.json) */
    FILE *mf = fopen(UIFS_CUR "/ui-manifest.json", "wb");
    if (mf) { fwrite(body, 1, got, mf); fclose(mf); }
    free(body);
    refresh_installed();

    httpd_resp_set_type(req, "application/json");
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"status\":\"OK\",\"files\":%d,\"installed\":%s}",
             verified, s_installed ? "true" : "false");
    httpd_resp_sendstr(req, resp);
    ESP_LOGI(TAG, "ui-commit OK: %d files verified, UI %s", verified, s_installed ? "LIVE" : "incomplete");
    return ESP_OK;
}
