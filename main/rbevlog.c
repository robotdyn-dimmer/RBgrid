/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbevlog.c
 * @brief On-hub event log: PSRAM ring of diag lines, rotated LittleFS files, HTTP read-out.
 *
 * See rbevlog.h for the public API.
 *
 * Ring (PSRAM, this boot): every diag line, incl. the periodic reconciliation trace.
 * Files (/logs/ev0.log newest … ev5.log oldest, 64 KB each, rotated): event + FAIL lines only, one per
 * text line: "<seq> <boot> <ts> <up_s> <lvl> <msg>". seq is monotonic across boots (continued from the
 * newest file at attach), boot counts hub boots seen by the log. ts = wall clock (0 = unknown; lines
 * written before NTP get their ts back-filled from uptime at the first flush with a valid clock).
 *
 * Calling contexts: rbevlog_add() is called from any task that emits a diag line (guarded by s_mtx, 50 ms
 * timeout, then the line is counted as dropped); the writer task flushes to the files every EV_FLUSH_MS or
 * at once on a FAIL line; the HTTP handlers run in the httpd task.
 */
#include "rbstrutil.h"     /* rb_scat — bounded snprintf append */
#include "rbevlog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "cJSON.h"
#include "rbpower.h"
#include "rbhome_types.h"
#include "rbwebserver.h"
#if CONFIG_RBGRID_BENCH_FULL || CONFIG_RBGRID_DEBUG_BRIDGE
#include "rbbench.h"
/** @brief 1 when POST /api/v1/debug/cmd (remote console verbs) is compiled in (debug builds). */
#define RBEVLOG_REMOTE_CMD 1
#else
/** @brief 0: remote console verbs are not compiled in (release builds). */
#define RBEVLOG_REMOTE_CMD 0
#endif

/*  @brief Add the web server's CORS headers to a response (rbwebserver CORS policy).
 *  @param[in] req  HTTP request. */
extern void add_cors_headers(httpd_req_t *req);   /* rbwebserver CORS policy */

static const char *TAG = "rbevlog";

#define EV_MSG        200             ///< max line length (bytes, incl. NUL)
#define EV_SLOTS      1536            ///< ring lines (~320 KB PSRAM); halved on allocation failure
#define EV_DIR        "/logs"         ///< mount point of the `logs` partition
#define EV_FILES      6               ///< number of rotated files (ev0 newest .. ev5 oldest)
#define EV_FILE_MAX   (64 * 1024)     ///< rotate when ev0.log reaches this size (bytes)
#define EV_BATCH      (16 * 1024)     ///< file write batch buffer (bytes, PSRAM)
#define EV_FLUSH_MS   30000           ///< periodic flush interval (ms)
#define EV_NTP_WAIT_S 180             ///< hold the first flush this long (s) for NTP (boot lines get a ts)
#define EV_SEQ_GUARD  1000            ///< seq jump at boot: past unflushed lines of a power-cut boot

/** @brief Line level: periodic reconciliation trace (RAM only), event, or FAIL invariant. */
enum { LVL_PERIODIC = 0, LVL_EVENT = 1, LVL_FAIL = 2 };
static const char *const LVL_NAME[] = { "periodic", "event", "fail" };

/** @brief One ring line. */
typedef struct {
    uint32_t seq, ts, up_s;
    uint8_t  lvl;
    char     msg[EV_MSG];
} ev_slot_t;
/** @var ev_slot_t::seq
 *  Monotonic sequence number (across boots).  @var ev_slot_t::ts
 *  Wall clock (Unix s), 0 = unknown.  @var ev_slot_t::up_s
 *  Uptime (s).  @var ev_slot_t::lvl
 *  LVL_PERIODIC / LVL_EVENT / LVL_FAIL.  @var ev_slot_t::msg
 *  Line text (CR/LF replaced by spaces). */

static ev_slot_t        *s_ring;
static uint32_t          s_nslots;
static SemaphoreHandle_t s_mtx;
static uint32_t          s_base;        /* seq of line k = s_base + k + 1 */
static uint32_t          s_count;       /* lines added this boot */
static uint32_t          s_persist_k;   /* lines [0, s_persist_k) already considered for the files */
static uint32_t          s_boot;
static uint32_t          s_dropped;
static uint32_t          s_fail_boot;
static bool              s_persist_ok;
static bool              s_ts_fixed;
static TaskHandle_t      s_writer;
static char             *s_batch;

static inline uint32_t up_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000LL); }
static inline uint32_t oldest_k(void) { return s_count > s_nslots ? s_count - s_nslots : 0; }

/** Classify a line: "FAIL ..." → LVL_FAIL, the periodic reconciliation trace → LVL_PERIODIC, else LVL_EVENT. */
static uint8_t classify(const char *m)
{
    if (!strncmp(m, "FAIL ", 5)) return LVL_FAIL;
    /* periodic reconciliation: "m<N> +<s>s cnt=" */
    const char *p = m;
    if (*p++ != 'm' || !isdigit((unsigned char)*p)) return LVL_EVENT;
    while (isdigit((unsigned char)*p)) p++;
    if (p[0] != ' ' || p[1] != '+' || !isdigit((unsigned char)p[2])) return LVL_EVENT;
    p += 2;
    while (isdigit((unsigned char)*p)) p++;
    return strncmp(p, "s cnt=", 6) ? LVL_EVENT : LVL_PERIODIC;
}

static void log_sink(const char *line) { rbevlog_add(line); }

void rbevlog_init(void)
{
    if (s_ring) return;
    s_mtx = xSemaphoreCreateMutex();
    for (uint32_t n = EV_SLOTS; n >= 128 && !s_ring; n /= 2) {
        s_ring = heap_caps_calloc(n, sizeof(ev_slot_t), MALLOC_CAP_SPIRAM);
        if (s_ring) s_nslots = n;
    }
    s_batch = heap_caps_malloc(EV_BATCH, MALLOC_CAP_SPIRAM);
    if (!s_ring || !s_mtx || !s_batch) { ESP_LOGE(TAG, "no memory — event log off"); free(s_ring); s_ring = NULL; return; }
    rbpower_diag_set_log_sink(log_sink);
    ESP_LOGI(TAG, "ring %lu lines (%u KB PSRAM)", (unsigned long)s_nslots,
             (unsigned)(s_nslots * sizeof(ev_slot_t) / 1024));
}

void rbevlog_add(const char *msg)
{
    if (!s_ring || !msg) return;
    uint32_t ts = rbtime_is_time_valid() ? (uint32_t)rbtime_get_unix_timestamp() : 0;
    uint8_t lvl = classify(msg);
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(50)) != pdTRUE) { s_dropped++; return; }
    uint32_t k = s_count++;
    ev_slot_t *sl = &s_ring[k % s_nslots];
    sl->seq = s_base + k + 1; sl->ts = ts; sl->up_s = up_s(); sl->lvl = lvl;
    strlcpy(sl->msg, msg, sizeof(sl->msg));
    for (char *c = sl->msg; *c; c++) if (*c == '\r' || *c == '\n') *c = ' ';
    if (lvl == LVL_FAIL) s_fail_boot++;
    xSemaphoreGive(s_mtx);
    if (lvl == LVL_FAIL && s_writer) xTaskNotifyGive(s_writer);
}

/* ---- files ---- */
static void ev_path(char *p, size_t n, int i) { snprintf(p, n, EV_DIR "/ev%d.log", i); }

/** Rotate the files when ev0.log reached EV_FILE_MAX: drop the oldest, shift ev<i> to ev<i+1>. */
static void rotate_if_full(void)
{
    char p0[24]; ev_path(p0, sizeof(p0), 0);
    struct stat st;
    if (stat(p0, &st) != 0 || st.st_size < EV_FILE_MAX) return;
    char a[24], b[24];
    ev_path(a, sizeof(a), EV_FILES - 1); unlink(a);
    for (int i = EV_FILES - 2; i >= 0; i--) {
        ev_path(a, sizeof(a), i); ev_path(b, sizeof(b), i + 1);
        rename(a, b);
    }
}

/** Append the not-yet-persisted event/FAIL lines to ev0.log (in EV_BATCH chunks). Unless @p force, waits up to
 *  EV_NTP_WAIT_S after boot for a valid clock, then back-fills the wall time of earlier lines. */
static void flush_once(bool force)
{
    if (!s_ring) return;
    bool tv = rbtime_is_time_valid();
    uint32_t now_up = up_s();
    if (!force && !tv && now_up < EV_NTP_WAIT_S) return;
    for (;;) {
        size_t len = 0;
        bool more = false;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        if (tv && !s_ts_fixed) {   /* back-fill wall time of the lines logged before NTP */
            uint32_t now = (uint32_t)rbtime_get_unix_timestamp();
            for (uint32_t k = oldest_k(); k < s_count; k++) {
                ev_slot_t *sl = &s_ring[k % s_nslots];
                if (!sl->ts && now > now_up - sl->up_s) sl->ts = now - (now_up - sl->up_s);
            }
            s_ts_fixed = true;
        }
        uint32_t k = s_persist_k < oldest_k() ? oldest_k() : s_persist_k;
        for (; k < s_count; k++) {
            ev_slot_t *sl = &s_ring[k % s_nslots];
            if (sl->lvl == LVL_PERIODIC) continue;
            if (len + EV_MSG + 64 > EV_BATCH) { more = true; break; }
            len = rb_scat(s_batch, EV_BATCH, len, "%lu %lu %lu %lu %u %s\n",
                            (unsigned long)sl->seq, (unsigned long)s_boot, (unsigned long)sl->ts,
                            (unsigned long)sl->up_s, (unsigned)sl->lvl, sl->msg);
        }
        s_persist_k = k;
        xSemaphoreGive(s_mtx);
        if (len && s_persist_ok) {
            char p0[24]; ev_path(p0, sizeof(p0), 0);
            FILE *f = fopen(p0, "a");
            if (f) { fwrite(s_batch, 1, len, f); fclose(f); rotate_if_full(); }
            else ESP_LOGW(TAG, "append %s failed", p0);
        }
        if (!more) break;
    }
}

/** Writer task: logs the running image id once, then flushes every EV_FLUSH_MS or when notified (FAIL line). */
static void writer_task(void *arg)
{
    (void)arg;
    {   /* which firmware image booted: md5 of the running image (first 8 hex = image id), once per boot */
        const esp_partition_t *run = esp_ota_get_running_partition();
        const char *md5 = rbwebserver_running_image_md5();
        char l[96];
        snprintf(l, sizeof(l), "fw: image %.8s (md5 %s) on '%s'", md5[0] ? md5 : "?", md5, run ? run->label : "?");
        rbevlog_add(l);
    }
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(EV_FLUSH_MS));
        flush_once(false);
    }
}

/* esp_restart path: the last lines (e.g. "ota: … rebooting") must reach the file before the reset */
static void shutdown_flush(void) { flush_once(true); }

/* last "<seq> <boot>" in the newest non-empty file */
static bool read_last(uint32_t *seq, uint32_t *boot)
{
    for (int i = 0; i < EV_FILES; i++) {
        char p[24]; ev_path(p, sizeof(p), i);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        char tail[640];
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        long off = sz > (long)sizeof(tail) - 1 ? sz - (long)sizeof(tail) + 1 : 0;
        fseek(f, off, SEEK_SET);
        size_t n = fread(tail, 1, sizeof(tail) - 1, f);
        fclose(f);
        tail[n] = 0;
        while (n && (tail[n - 1] == '\n' || tail[n - 1] == '\r')) tail[--n] = 0;
        if (!n) continue;
        char *line = strrchr(tail, '\n');
        line = line ? line + 1 : tail;
        unsigned long s = 0, b = 0;
        if (sscanf(line, "%lu %lu", &s, &b) == 2) { *seq = s; *boot = b; return true; }
    }
    return false;
}

void rbevlog_attach_storage(void)
{
    if (!s_ring || s_writer) return;
    if (esp_littlefs_mounted("logs")) {
        s_persist_ok = true;
    } else {
        esp_vfs_littlefs_conf_t conf = {
            .base_path = EV_DIR, .partition_label = "logs",
            .format_if_mount_failed = true, .dont_mount = false,
        };
        esp_err_t e = esp_vfs_littlefs_register(&conf);
        s_persist_ok = (e == ESP_OK);
        if (e != ESP_OK) ESP_LOGW(TAG, "mount logs: %s — RAM only", esp_err_to_name(e));
    }
    uint32_t last_seq = 0, last_boot = 0;
    if (s_persist_ok) read_last(&last_seq, &last_boot);
    /* Lines of the previous boot that never reached the file (power cut before a flush) may already have
     * been SEEN by a client (seq cursor) — start this boot past anything that boot could have numbered so a
     * `since=` cursor never skips new lines; the jump is visible as a seq gap, not as reuse. */
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_base = last_seq ? last_seq + EV_SEQ_GUARD : 0;
    s_boot = last_boot + 1;
    for (uint32_t k = oldest_k(); k < s_count; k++) s_ring[k % s_nslots].seq = s_base + k + 1;
    xSemaphoreGive(s_mtx);
    xTaskCreate(writer_task, "rbevlog", 4096, NULL, 2, &s_writer);
    esp_register_shutdown_handler(shutdown_flush);   /* every esp_restart (reboot API, OTA, verbs) */
    char l[96];
    snprintf(l, sizeof(l), "evlog: boot %lu, seq from %lu, files %s", (unsigned long)s_boot,
             (unsigned long)(s_base + 1), s_persist_ok ? "ok" : "OFF (RAM only)");
    rbevlog_add(l);
    /* reserve this boot number + keep the boot lines even if power goes before the first periodic flush
     * (which waits up to EV_NTP_WAIT_S for NTP); otherwise two quick power cycles can reuse a boot number */
    flush_once(true);
}

/* ---- HTTP: GET /api/v1/system/events ---- */
/** @brief Chunked JSON output state of one GET /api/v1/system/events response. */
typedef struct {
    httpd_req_t *req;
    char  *buf;
    size_t len, cap;
    uint32_t emitted, last_seq;
    bool   first;
} ev_out_t;
/** @var ev_out_t::req
 *  The HTTP request.  @var ev_out_t::buf
 *  Chunk buffer.  @var ev_out_t::len
 *  Bytes in @c buf.  @var ev_out_t::cap
 *  Size of @c buf.  @var ev_out_t::emitted
 *  Events written.  @var ev_out_t::last_seq
 *  seq of the last event written.  @var ev_out_t::first
 *  No event written yet (no leading comma). */

static void out_flush(ev_out_t *o)
{
    if (o->len) { httpd_resp_send_chunk(o->req, o->buf, o->len); o->len = 0; }
}
static void out_raw(ev_out_t *o, const char *s)
{
    size_t n = strlen(s);
    if (o->len + n >= o->cap) out_flush(o);
    memcpy(o->buf + o->len, s, n); o->len += n;
}
/** Write one event as a JSON object (msg escaped). */
static void out_event(ev_out_t *o, uint32_t seq, uint32_t b, uint32_t ts, uint32_t up, uint8_t lvl, const char *msg)
{
    char head[128];
    snprintf(head, sizeof(head), "%s{\"seq\":%lu,\"b\":%lu,\"ts\":%lu,\"up\":%lu,\"lvl\":\"%s\",\"msg\":\"",
             o->first ? "" : ",", (unsigned long)seq, (unsigned long)b, (unsigned long)ts, (unsigned long)up,
             LVL_NAME[lvl > LVL_FAIL ? LVL_FAIL : lvl]);
    out_raw(o, head);
    char esc[EV_MSG * 2 + 8]; size_t e = 0;
    for (const char *c = msg; *c && e < sizeof(esc) - 8; c++) {
        unsigned char ch = (unsigned char)*c;
        if (ch == '"' || ch == '\\') { esc[e++] = '\\'; esc[e++] = ch; }
        else if (ch < 0x20) e = rb_scat(esc, sizeof(esc), e, "\\u%04x", ch);
        else esc[e++] = ch;
    }
    esc[e] = 0;
    out_raw(o, esc);
    out_raw(o, "\"}");
    o->first = false; o->emitted++; o->last_seq = seq;
}

static bool qparam_u32(const char *q, const char *key, uint32_t *v)
{
    char s[16];
    if (httpd_query_key_value(q, key, s, sizeof(s)) != ESP_OK) return false;
    char *end; unsigned long x = strtoul(s, &end, 10);
    if (end == s || *end) return false;
    *v = (uint32_t)x; return true;
}

/** GET /api/v1/system/events?since=&limit=&level=all|event|fail — files (when `since` is older than the ring)
 *  then the ring; without `since`, the last `limit` lines of the ring. Runs in the httpd task. */
static esp_err_t events_get(httpd_req_t *req)
{
    add_cors_headers(req);
    if (!rbwebserver_api_check_read(req)) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    if (!s_ring) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"code\":\"EVLOG_OFF\",\"message\":\"event log not initialized\"}");
        return ESP_OK;
    }
    char q[96] = {0};
    uint32_t since = 0, limit = 200;
    bool has_since = false;
    uint8_t minlvl = LVL_PERIODIC;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        has_since = qparam_u32(q, "since", &since);
        qparam_u32(q, "limit", &limit);
        char lv[12];
        if (httpd_query_key_value(q, "level", lv, sizeof(lv)) == ESP_OK) {
            if (!strcmp(lv, "event")) minlvl = LVL_EVENT;
            else if (!strcmp(lv, "fail")) minlvl = LVL_FAIL;
            else if (strcmp(lv, "all")) {
                httpd_resp_set_status(req, "400 Bad Request");
                httpd_resp_sendstr(req, "{\"status\":\"error\",\"code\":\"BAD_LEVEL\",\"message\":\"level = all|event|fail\"}");
                return ESP_OK;
            }
        }
    }
    if (limit < 1) limit = 1;
    if (limit > 1000) limit = 1000;

    ev_out_t o = { .req = req, .cap = 4096, .first = true };
    o.buf = heap_caps_malloc(o.cap, MALLOC_CAP_SPIRAM);
    if (!o.buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mem"); return ESP_FAIL; }
    out_raw(&o, "{\"events\":[");

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    uint32_t base = s_base, count = s_count, ok = oldest_k();
    xSemaphoreGive(s_mtx);
    uint32_t ram_first = base + ok + 1;          /* oldest seq still in the ring */
    uint32_t seq_last = base + count;
    bool more = false;
    uint32_t start_k = ok;

    if (has_since) {
        /* older than the ring → the files first (event/FAIL lines of this and earlier boots) */
        if (since + 1 < ram_first && s_persist_ok) {
            char line[EV_MSG + 64];
            for (int i = EV_FILES - 1; i >= 0 && !more; i--) {
                char p[24]; ev_path(p, sizeof(p), i);
                FILE *f = fopen(p, "r");
                if (!f) continue;
                while (fgets(line, sizeof(line), f)) {
                    unsigned long sq, b, ts, up; unsigned lvl; int off = 0;
                    if (sscanf(line, "%lu %lu %lu %lu %u %n", &sq, &b, &ts, &up, &lvl, &off) < 5 || !off) continue;
                    if (sq <= since || sq >= ram_first || lvl < minlvl) continue;
                    if (o.emitted >= limit) { more = true; break; }
                    char *msg = line + off;
                    size_t ml = strlen(msg);
                    while (ml && (msg[ml - 1] == '\n' || msg[ml - 1] == '\r')) msg[--ml] = 0;
                    out_event(&o, sq, b, ts, up, (uint8_t)lvl, msg);
                }
                fclose(f);
            }
        }
        start_k = (since > base) ? since - base : 0;
        if (start_k < ok) start_k = ok;
    } else {
        /* tail: the last `limit` matching lines of the ring */
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        uint32_t n = 0, k = s_count;
        while (k > oldest_k() && n < limit) {
            k--;
            if (s_ring[k % s_nslots].lvl >= minlvl) n++;
        }
        start_k = k;
        xSemaphoreGive(s_mtx);
    }
    /* the ring, copied out a few slots at a time (never stream under the lock) */
    ev_slot_t tmp[8];
    for (uint32_t k = start_k; k < count && !more; ) {
        uint32_t got = 0;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        if (k < oldest_k()) k = oldest_k();   /* overwritten meanwhile */
        while (got < 8 && k + got < count) { tmp[got] = s_ring[(k + got) % s_nslots]; got++; }
        xSemaphoreGive(s_mtx);
        for (uint32_t i = 0; i < got; i++) {
            if (tmp[i].lvl < minlvl) continue;
            if (o.emitted >= limit) { more = true; break; }
            out_event(&o, tmp[i].seq, s_boot, tmp[i].ts, tmp[i].up_s, tmp[i].lvl, tmp[i].msg);
        }
        k += got;
        if (!got) break;
    }
    char tail[320];
    uint32_t now = rbtime_is_time_valid() ? (uint32_t)rbtime_get_unix_timestamp() : 0;
    snprintf(tail, sizeof(tail),
             "],\"boot\":%lu,\"seq_last\":%lu,\"ram_first\":%lu,\"next\":%lu,\"more\":%s,"
             "\"persist\":%s,\"dropped\":%lu,\"fail_boot\":%lu,\"up\":%lu,\"now\":%lu}",
             (unsigned long)s_boot, (unsigned long)seq_last, (unsigned long)ram_first,
             (unsigned long)(o.emitted ? o.last_seq : (has_since ? since : seq_last)),
             more ? "true" : "false", s_persist_ok ? "true" : "false",
             (unsigned long)s_dropped, (unsigned long)s_fail_boot, (unsigned long)up_s(), (unsigned long)now);
    out_raw(&o, tail);
    out_flush(&o);
    httpd_resp_send_chunk(req, NULL, 0);
    free(o.buf);
    return ESP_OK;
}

#if RBEVLOG_REMOTE_CMD
/* ---- HTTP: POST /api/v1/debug/cmd — run one console verb remotely (token-guarded) ----
 * Refused over the network: verbs that move the clock, inject synthetic data, erase/force stats,
 * change calibration/config, touch secrets, cut the link, or write raw I2C. They stay on the USB console. */
static const char *const DENY[] = {
    "clock", "tarifftime", "teletime", "flasherase", "stattest", "statfeed", "statforce1h", "forceperiod",
    "phfeed", "gridfeed", "injtest", "inj200", "vmod", "calgain", "token", "webhttps", "rmunit", "flashen",
    "shutdown", "leakprobe", "wifidrop", "i2cset", "i2cwrite", "load", "ctsave", "progmode", "progtier", "crashtest",
};
/* Verbs that stop metering, write module calibration/config or switch the ESP-NOW link: allowed over the network
 * only on a hub with an admin token — on an unarmed (onboarding) hub anyone on the LAN could run them. */
static const char *const TOKEN_ONLY[] = { "pollpause", "savecal", "espnow", "libcheck", "appw", "ctmodel", "modreset" };
extern bool rbwebserver_api_token_armed(void);
/** Check a remote verb against DENY / TOKEN_ONLY and the special cases; on refusal write the reason to @p why. */
static bool cmd_allowed(const char *line, char *why, size_t wn)
{
    char v[24] = {0}, a1[16] = {0};
    sscanf(line, "%23s %15s", v, a1);
    for (char *c = v; *c; c++) *c = tolower((unsigned char)*c);
    for (size_t i = 0; i < sizeof(DENY) / sizeof(DENY[0]); i++)
        if (!strcmp(v, DENY[i])) { snprintf(why, wn, "verb '%s' is console-only", v); return false; }
    for (size_t i = 0; i < sizeof(TOKEN_ONLY) / sizeof(TOKEN_ONLY[0]); i++)
        if (!strcmp(v, TOKEN_ONLY[i]) && !rbwebserver_api_token_armed()) {
            snprintf(why, wn, "verb '%s' needs an admin token on the hub (POST /api/system/token)", v); return false;
        }
    if (!strcmp(v, "reboot")) {   /* would restart inside this request — the reply never leaves */
        snprintf(why, wn, "use POST /api/system/reboot"); return false;
    }
    if (!strcmp(v, "rbampcfg") && a1[0] && strcasecmp(a1, "show") && strcasecmp(a1, "count")) {
        snprintf(why, wn, "rbampcfg %s is console-only (show/count are allowed)", a1); return false;
    }
    return v[0] != 0;
}

/** POST /api/v1/debug/cmd: body {"cmd":"<verb args>"} or plain text; runs the verb through rbbench_exec and
 *  returns its output as JSON. */
static esp_err_t cmd_post(httpd_req_t *req)
{
    add_cors_headers(req);
    if (!rbwebserver_api_check_auth(req)) return ESP_FAIL;   /* 401 already sent */
    httpd_resp_set_type(req, "application/json");
    char body[192] = {0};
    int rl = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    if (rl <= 0 || httpd_req_recv(req, body, rl) <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"code\":\"NO_CMD\",\"message\":\"body = {\\\"cmd\\\":\\\"<verb args>\\\"} or plain text\"}");
        return ESP_OK;
    }
    char line[160] = {0};
    if (body[0] == '{') {
        cJSON *j = cJSON_Parse(body);
        cJSON *c = j ? cJSON_GetObjectItem(j, "cmd") : NULL;
        if (cJSON_IsString(c)) strlcpy(line, c->valuestring, sizeof(line));
        cJSON_Delete(j);
    } else {
        strlcpy(line, body, sizeof(line));
    }
    for (char *c = line; *c; c++) if (*c == '\r' || *c == '\n') *c = ' ';
    char why[80];
    if (!cmd_allowed(line, why, sizeof(why))) {
        httpd_resp_set_status(req, "403 Forbidden");
        char r[192];
        snprintf(r, sizeof(r), "{\"status\":\"error\",\"code\":\"CMD_FORBIDDEN\",\"message\":\"%s\"}", line[0] ? why : "empty cmd");
        httpd_resp_sendstr(req, r);
        return ESP_OK;
    }
    rbpower_diag_printf("remote cmd: %s", line);
    const size_t cap = 8192;
    char *out = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!out) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mem"); return ESP_FAIL; }
    bool trunc = false;
    int n = rbbench_exec(line, out, cap, &trunc);
    if (n < 0) {
        free(out);
        httpd_resp_set_status(req, n == -2 ? "409 Conflict" : "503 Service Unavailable");
        httpd_resp_sendstr(req, n == -2 ? "{\"status\":\"error\",\"code\":\"BUSY\",\"message\":\"another console verb is running\"}"
                                        : "{\"status\":\"error\",\"code\":\"NO_CONSOLE\",\"message\":\"console not initialized\"}");
        return ESP_OK;
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    cJSON_AddStringToObject(r, "cmd", line);
    cJSON_AddStringToObject(r, "output", out);
    cJSON_AddBoolToObject(r, "truncated", trunc);
    free(out);
    char *s = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (!s) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json"); return ESP_FAIL; }
    httpd_resp_sendstr(req, s);
    free(s);
    return ESP_OK;
}
#endif

void rbevlog_register_http(void)
{
    httpd_handle_t srv = rbwebserver_core_server();
    if (!srv) { ESP_LOGW(TAG, "no server handle"); return; }
    httpd_uri_t g = { .uri = "/api/v1/system/events", .method = HTTP_GET, .handler = events_get, .user_ctx = NULL };
    esp_err_t eg = httpd_register_uri_handler(srv, &g);
    esp_err_t ec = ESP_OK;
#if RBEVLOG_REMOTE_CMD
    httpd_uri_t c = { .uri = "/api/v1/debug/cmd", .method = HTTP_POST, .handler = cmd_post, .user_ctx = NULL };
    ec = httpd_register_uri_handler(srv, &c);
#endif
    if (eg != ESP_OK || ec != ESP_OK)
        ESP_LOGE(TAG, "URI registration FAILED (events=%s cmd=%s) — check max_uri_handlers",
                 esp_err_to_name(eg), esp_err_to_name(ec));
}
