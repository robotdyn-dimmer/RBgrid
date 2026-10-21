// rbbench — development/test-rig debug console for RBgrid (generic half).
// See include/rbbench.h. ESP-IDF v5.x (new i2c_master + uart driver).
#include "rbbench.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>   /* atoi — i2cscan [bus] */
#include <stdarg.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"   /* esp_rom_printf — always hits UART0 (bypasses console/driver) */

// ---- test-rig wiring -------------------------------------------------------
#define RBBENCH_UART          UART_NUM_0          // USB console, COM6
#define RBBENCH_UART_BUF      512
#define RBBENCH_LINE_MAX      160
#define RBBENCH_MAX_CMDS      64   // was 16→32 — main.cpp registers 20 verbs; the cap SILENTLY dropped
                                   // #17-20 (statread/statcursor/stattest/flasherase), so they were
                                   // absent from `help` and ran as unknown→no-op. This caused a multi-
                                   // phantom "store bug": statread returned
                                   // nothing because the verb wasn't registered, not because flash was
                                   // empty. Headroom for growth + register now warns on overflow.
#define LOAD_SDA              19                  // load bus — moved off 21/22 (a live-rbAmp
#define LOAD_SCL              23                  // physical crossover killed it there; 19/23
                                                  // free on bench, clear of rbAmp/ext-flash/DW-81)
#define LOAD_HZ               50000               // marginal bus → 50k
#define SENSOR_SDA            25                  // DUT/rbAmp bus (self-created if app didn't)
#define SENSOR_SCL            26
#define LOAD_ADDR_A           0x20                // PCF8574 bank A
#define LOAD_ADDR_B           0x25                // PCF8574 bank B
#define SENSOR_HZ             50000               // rbAmp @ 50k (SPEC §B.5)
#define I2C_TMO_MS            60
// DW-81 reference meter — RS485 Modbus RTU on UART2 (auto-direction MAX485).
// Moved off GPIO16/17 (now reserved by PSRAM on the WROVER platform). NOT 13/14 either —
// those are the external NOR-flash HSPI bus (MOSI=13/SCLK=14). 27/32 are clear of both SPI
// buses + PSRAM (future-proof pair). Bench-only / debug meter.
#define RS485_UART            UART_NUM_2
#define RS485_TX              27
#define RS485_RX              32
#define DW81_ADDR             0x01                // Modbus slave addr, 9600 8N2

// DUT control/status lines (test-rig PCB pinout).
// Reserved here so nothing else reuses the pins; wire up (init + verb) on demand.
#define DUT_MODE_PIN          4                   // Mode → DUT (ESP32 output, drives rbAmp mode select)
#define DUT_DRDY_PIN          33                  // DRDY ← DUT (ESP32 input, rbAmp data-ready). Was native-ADC
                                                  // CH7 (disabled/unconnected) — now repurposed as DRDY input.

static const char *TAG = "rbbench";
static i2c_master_bus_handle_t s_sensor_bus;      // borrowed from RBgrid (25/26)
static i2c_master_bus_handle_t s_load_bus;        // owned by rbbench (21/22)
static uint8_t s_load_a, s_load_b;                // shadow

struct { const char *name; rbbench_cmd_fn fn; } s_cmds[RBBENCH_MAX_CMDS];
static int s_ncmds;
static int s_cmds_dropped;   // verbs lost to a full table — surfaced in `help` + unknown-verb replies

// ---- output ---------------------------------------------------------------
// Remote execution (rbbench_exec — for hubs without a USB console): while a verb runs
// for a remote caller, replies FROM THAT TASK go to the caller's buffer instead of UART0. Replies from
// other tasks (#DIAG trace etc.) keep going to the UART. s_exec_mtx serializes every dispatch (UART and
// remote) so verbs never run concurrently with each other.
static SemaphoreHandle_t s_exec_mtx;
static TaskHandle_t s_cap_task;
static char  *s_cap_buf;
static size_t s_cap_cap, s_cap_len;
static bool   s_cap_trunc;

void rbbench_reply(const char *fmt, ...) {
    char buf[RBBENCH_LINE_MAX + 4];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 3, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    /* vsnprintf(buf, sizeof-3) holds at most sizeof-4 chars + NUL: clamping to sizeof-3 copied the NUL into
     * the captured output and everything after it vanished from the JSON (long i2cscan line) */
    if (n > (int)sizeof(buf) - 4) n = sizeof(buf) - 4;
    if (s_cap_task && s_cap_task == xTaskGetCurrentTaskHandle()) {
        if (s_cap_len + n + 2 > s_cap_cap) { s_cap_trunc = true; return; }
        memcpy(s_cap_buf + s_cap_len, buf, n);
        s_cap_len += n;
        s_cap_buf[s_cap_len++] = '\n';
        s_cap_buf[s_cap_len] = 0;
        return;
    }
    buf[n++] = '\r'; buf[n++] = '\n';
    uart_write_bytes(RBBENCH_UART, buf, n);
}

// ---- parsing --------------------------------------------------------------
bool rbbench_parse_uint(const char *s, uint32_t *out, uint32_t max_val) {
    if (!s || *s == '\0' || s[0] == '+' || s[0] == '-') return false;
    int base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; if (*s == '\0') return false; }
    char *end = NULL;
    unsigned long v = strtoul(s, &end, base);
    if (end == s || *end != '\0' || v > max_val) return false;
    *out = (uint32_t)v;
    return true;
}

// Shared sensor-bus lock — provided by the integrating app (rbgrid's
// rbamp_bridge exports rbamp_bridge_bus_lock/unlock; rbamp_poll holds it around
// rbamp_read_all). Weak no-op fallback keeps rbbench linkable standalone; when
// the real ones are present the shared bus (25/26) is collision-safe so raw
// i2cget/i2cset/i2cwrite/i2cscan can't wedge the IDF i2c_master driver.
__attribute__((weak)) void rbamp_bridge_bus_lock(void) {}
__attribute__((weak)) void rbamp_bridge_bus_unlock(void) {}

// ---- i2c helpers (add/remove a temp device per op — simple + robust) ------
static esp_err_t i2c_rd(i2c_master_bus_handle_t bus, uint8_t addr, uint32_t hz,
                        const uint8_t *wr, size_t wn, uint8_t *rd, size_t rn) {
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                               .device_address = addr, .scl_speed_hz = hz };
    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(bus, &dc, &dev) != ESP_OK) return ESP_FAIL;
    esp_err_t e = rn ? i2c_master_transmit_receive(dev, wr, wn, rd, rn, I2C_TMO_MS)
                     : i2c_master_transmit(dev, wr, wn, I2C_TMO_MS);
    i2c_master_bus_rm_device(dev);
    return e;
}

static bool load_write(uint8_t addr, uint8_t val) {       // PCF8574, retry NACK
    for (int i = 0; i < 6; i++) {
        if (i2c_rd(s_load_bus, addr, LOAD_HZ, &val, 1, NULL, 0) == ESP_OK) return true;
        vTaskDelay(pdMS_TO_TICKS(3));
    }
    return false;
}

// ---- generic verbs --------------------------------------------------------
static void v_ping(int c, char **a)  { (void)c;(void)a; rbbench_reply("pong"); }

static void v_i2cget(int argc, char **argv) {
    uint32_t addr, reg, cnt = 1;
    if (argc < 3) { rbbench_reply("err i2cget needs <addr> <reg>"); return; }
    if (!rbbench_parse_uint(argv[1], &addr, 0xFF) || addr > 0x7F) { rbbench_reply("err i2cget addr 0..7F"); return; }
    if (!rbbench_parse_uint(argv[2], &reg, 0xFF)) { rbbench_reply("err i2cget reg 0..FF"); return; }
    if (argc >= 4 && (!rbbench_parse_uint(argv[3], &cnt, 32) || cnt < 1)) { rbbench_reply("err i2cget count 1..32"); return; }
    uint8_t r = (uint8_t)reg, buf[32];
    rbamp_bridge_bus_lock();
    esp_err_t e = i2c_rd(s_sensor_bus, addr, SENSOR_HZ, &r, 1, buf, cnt);
    rbamp_bridge_bus_unlock();
    if (e != ESP_OK) { rbbench_reply("err i2cget i2c rc=-1"); return; }
    char out[160]; int p = 0;
    for (uint32_t i = 0; i < cnt; i++) p += snprintf(out + p, sizeof(out) - p, i ? " 0x%02X" : "0x%02X", buf[i]);
    rbbench_reply("%s", out);
}

static void v_i2cset(int argc, char **argv) {
    uint32_t addr, reg, val;
    if (argc < 4) { rbbench_reply("err i2cset needs <addr> <reg> <val>"); return; }
    if (!rbbench_parse_uint(argv[1], &addr, 0xFF) || addr > 0x7F) { rbbench_reply("err i2cset addr 0..7F"); return; }
    if (!rbbench_parse_uint(argv[2], &reg, 0xFF)) { rbbench_reply("err i2cset reg 0..FF"); return; }
    if (!rbbench_parse_uint(argv[3], &val, 0xFF)) { rbbench_reply("err i2cset val 0..FF"); return; }
    uint8_t wr[2] = { (uint8_t)reg, (uint8_t)val };
    rbamp_bridge_bus_lock();
    esp_err_t e = i2c_rd(s_sensor_bus, addr, SENSOR_HZ, wr, 2, NULL, 0);
    rbamp_bridge_bus_unlock();
    rbbench_reply(e == ESP_OK ? "ok" : "err i2cset i2c rc=-1");
}

static void v_i2cwrite(int argc, char **argv) {   // raw N-byte, one transaction
    uint32_t addr;
    if (argc < 3) { rbbench_reply("err i2cwrite needs <addr> <b0> [b1..]"); return; }
    if (!rbbench_parse_uint(argv[1], &addr, 0xFF) || addr > 0x7F) { rbbench_reply("err i2cwrite addr 0..7F"); return; }
    uint8_t buf[24]; int n = 0;
    for (int i = 2; i < argc && n < (int)sizeof(buf); i++) {
        uint32_t b; if (!rbbench_parse_uint(argv[i], &b, 0xFF)) { rbbench_reply("err i2cwrite byte 0..FF"); return; }
        buf[n++] = (uint8_t)b;
    }
    rbamp_bridge_bus_lock();
    esp_err_t e = i2c_rd(s_sensor_bus, addr, SENSOR_HZ, buf, n, NULL, 0);
    rbamp_bridge_bus_unlock();
    rbbench_reply(e == ESP_OK ? "ok" : "err i2cwrite i2c rc=-1");
}

static void v_i2cscan(int c, char **a) {
    // i2cscan [bus] : 0 = sensor/rbAmp bus (25/26, default), 1 = load bus (21/22).
    // Lets the test rig discover what actually sits on the load bus (e.g. PCF8574 @0x20-0x27 vs
    // PCF8574A @0x38-0x3F) instead of guessing — the `load` verb hardcodes 0x20/0x25.
    // i2cscan [bus] [r] : `r` = READ probe (address + R/W=1, read 1 byte) instead of the write probe —
    // tells a real ACK from a bus that is merely held low after R/W=0 (isolator side-1 "zero", weak
    // pull-ups: ALL-ACK seen with some modules).
    int bus = (c >= 2) ? atoi(a[1]) : 0;
    bool rd = (c >= 3 && (a[2][0] == 'r' || a[2][0] == 'R'));
    uint32_t hz = (bus == 1) ? LOAD_HZ : SENSOR_HZ;   /* read mode: `i2cscan 0 r <hz>` (e.g. 10000 — rise-time test) */
    if (rd && c >= 4) { uint32_t v; if (rbbench_parse_uint(a[3], &v, 400000) && v >= 1000) hz = v; }
    i2c_master_bus_handle_t h = (bus == 1) ? s_load_bus : s_sensor_bus;
    if (!h) { rbbench_reply("err bus %d not initialized", bus); return; }
    char out[200]; int p = 0; bool first = true; int found = 0;
    uint16_t hist[256] = {0};   /* read mode: which byte values the "ACKed" reads returned */
    if (bus == 0) rbamp_bridge_bus_lock();   // sensor bus is shared with the rbAmp poll task
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        bool ack;
        if (rd) {
            uint8_t b = 0;
            i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr,
                                       .scl_speed_hz = hz };
            i2c_master_dev_handle_t dev;
            ack = false;
            if (i2c_master_bus_add_device(h, &dc, &dev) == ESP_OK) {
                ack = (i2c_master_receive(dev, &b, 1, 50) == ESP_OK);
                if (ack) hist[b]++;
                i2c_master_bus_rm_device(dev);
            }
        } else {
            ack = (i2c_master_probe(h, addr, 50) == ESP_OK);
        }
        if (ack) {
            found++;
            if (p < (int)sizeof(out) - 8) { p += snprintf(out + p, sizeof(out) - p, first ? "0x%02X" : " 0x%02X", addr); first = false; }
        }
    }
    if (bus == 0) rbamp_bridge_bus_unlock();
    /* n= first: a bus where EVERY address ACKs (e.g. SDA held low) is a
     * hardware symptom (SDA held low / pull-ups) — the count says so even when the list is cut. */
    rbbench_reply("bus%d (%s) %s-probe%s: n=%d%s %s", bus, bus == 1 ? "load 19/23" : "sensor 25/26", rd ? "read" : "write",
                  rd ? (hz != SENSOR_HZ && hz != LOAD_HZ ? " (custom hz)" : "") : "", found,
                  found >= 100 ? " ALL-ACK (STD-module isolator front-end ACKs every address, or SDA stuck low — check presence with stdinfo, not the scan)" : "", first ? "(none)" : out);
    if (rd && found) {   /* byte histogram of the read probes: all 0x00 = SDA never rises; varied = real data */
        char hb[160]; int q = 0, kinds = 0;
        for (int v = 0; v < 256 && q < (int)sizeof(hb) - 16; v++)
            if (hist[v]) { kinds++; q += snprintf(hb + q, sizeof(hb) - q, " %02X:%u", v, hist[v]); }
        rbbench_reply("  read bytes (%d distinct):%s", kinds, hb);
    }
}

static void v_load(int argc, char **argv) {
    if (argc < 2) { rbbench_reply("err load needs off|status|<a> <b>"); return; }
    char k[12]; strncpy(k, argv[1], sizeof(k) - 1); k[sizeof(k)-1] = 0;
    for (char *s = k; *s; s++) *s = tolower((unsigned char)*s);
    if (!strcmp(k, "status")) { rbbench_reply("a=0x%02X b=0x%02X", s_load_a, s_load_b); return; }
    uint8_t a, b;
    if (!strcmp(k, "off")) { a = 0; b = 0; }
    else {
        uint32_t va, vb;
        if (argc < 3 || !rbbench_parse_uint(argv[1], &va, 0xFF) || !rbbench_parse_uint(argv[2], &vb, 0xFF)) {
            rbbench_reply("err load needs off|status|<a> <b>"); return; }
        a = (uint8_t)va; b = (uint8_t)vb;
    }
    if (load_write(LOAD_ADDR_A, a) & load_write(LOAD_ADDR_B, b)) { s_load_a = a; s_load_b = b; rbbench_reply("ok"); }
    else rbbench_reply("err load i2c");
}

// ---- DW-81 reference meter (RS485 Modbus RTU) -----------------------------
static uint16_t modbus_crc16(const uint8_t *buf, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    }
    return crc;
}
static float be_float(const uint8_t *p) {       // big-endian-on-wire -> host float
    union { uint8_t b[4]; float f; } u;
    u.b[0] = p[3]; u.b[1] = p[2]; u.b[2] = p[1]; u.b[3] = p[0];
    return u.f;
}
static void v_meter(int argc, char **argv) {
    (void)argc;(void)argv;
    // Block read 0x002A..0x0057 (46 regs): req = 01 03 00 2A 00 2E <crc>
    uint8_t req[8] = { DW81_ADDR, 0x03, 0x00, 0x2A, 0x00, 0x2E, 0, 0 };
    uint16_t crc = modbus_crc16(req, 6);
    req[6] = crc & 0xFF; req[7] = crc >> 8;
    uint8_t resp[97];                            // addr+func+count(0x5C=92)+92+crc(2)
    for (int attempt = 0; attempt < 2; attempt++) {
        uart_flush_input(RS485_UART);
        uart_write_bytes(RS485_UART, (const char *)req, 8);
        int n = uart_read_bytes(RS485_UART, resp, sizeof(resp), pdMS_TO_TICKS(400));
        if (n == (int)sizeof(resp) && resp[0] == DW81_ADDR && resp[1] == 0x03 && resp[2] == 0x5C) {
            uint16_t cc = modbus_crc16(resp, 95);
            if ((uint16_t)(resp[95] | (resp[96] << 8)) == cc) {
                const uint8_t *d = resp + 3;
                rbbench_reply("U=%.1f I=%.3f P=%.1f Q=%.1f PF=%.3f F=%.2f",
                              be_float(d + 0), be_float(d + 12), be_float(d + 24),
                              be_float(d + 40), be_float(d + 72), be_float(d + 88));
                return;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(60));
    }
    rbbench_reply("err meter rc=-1");            // auto-dir MAX485 can drop bytes — retry/repeat
}

static void v_help(int c, char **a) {
    (void)c;(void)a;
    rbbench_reply("rbbench: ping i2cget i2cset i2cwrite i2cscan[0|1] load meter [+RBgrid verbs]");
    for (int i = 0; i < s_ncmds; i++) rbbench_reply("  custom: %s", s_cmds[i].name);
    if (s_cmds_dropped > 0)
        rbbench_reply("!! %d verb(s) DROPPED at registration (table %d full) — raise RBBENCH_MAX_CMDS",
                      s_cmds_dropped, RBBENCH_MAX_CMDS);
}

// ---- registry + dispatch --------------------------------------------------
void rbbench_register_cmd(const char *name, rbbench_cmd_fn fn) {
    if (s_ncmds < RBBENCH_MAX_CMDS) {
        s_cmds[s_ncmds].name = name; s_cmds[s_ncmds].fn = fn; s_ncmds++;
    } else {
        // Never drop silently again — an over-cap verb is invisible in `help` and runs as
        // unknown→no-op, which reads exactly like a broken feature (the earlier phantom store bug).
        s_cmds_dropped++;
        ESP_LOGE(TAG, "rbbench cmd table FULL (%d) — verb '%s' DROPPED, raise RBBENCH_MAX_CMDS",
                 RBBENCH_MAX_CMDS, name ? name : "(null)");
        // ESP_LOG is invisible on this console — also shout on UART0 directly (ROM path works
        // even before the uart driver is up), so a dropped verb is visible at boot, not deduced.
        esp_rom_printf("!! rbbench: cmd table FULL — verb '%s' DROPPED (raise RBBENCH_MAX_CMDS)\n",
                       name ? name : "(null)");
    }
}

static void dispatch_locked(char *line);
static void dispatch(char *line) {
    if (s_exec_mtx) xSemaphoreTake(s_exec_mtx, portMAX_DELAY);
    dispatch_locked(line);
    if (s_exec_mtx) xSemaphoreGive(s_exec_mtx);
}

int rbbench_exec(const char *line, char *out, size_t cap, bool *truncated) {
    if (!line || !out || cap < 2) return -1;
    if (!s_exec_mtx) return -1;                       /* console not initialized */
    if (xSemaphoreTake(s_exec_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) return -2;   /* a verb is still running */
    char tmp[RBBENCH_LINE_MAX];
    strlcpy(tmp, line, sizeof(tmp));
    out[0] = 0;
    s_cap_buf = out; s_cap_cap = cap; s_cap_len = 0; s_cap_trunc = false;
    s_cap_task = xTaskGetCurrentTaskHandle();
    dispatch_locked(tmp);
    s_cap_task = NULL;
    int n = (int)s_cap_len;
    if (truncated) *truncated = s_cap_trunc;
    s_cap_buf = NULL;
    xSemaphoreGive(s_exec_mtx);
    return n;
}

static void dispatch_locked(char *line) {
    char *argv[12]; int argc = 0; char *p = line;
    while (*p && argc < 12) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    if (!argc) return;
    char *cmd = argv[0];
    for (char *s = cmd; *s; s++) *s = tolower((unsigned char)*s);
    if      (!strcmp(cmd, "ping"))     v_ping(argc, argv);
    else if (!strcmp(cmd, "help"))     v_help(argc, argv);
    else if (!strcmp(cmd, "i2cget"))   v_i2cget(argc, argv);
    else if (!strcmp(cmd, "i2cset"))   v_i2cset(argc, argv);
    else if (!strcmp(cmd, "i2cwrite")) v_i2cwrite(argc, argv);
    else if (!strcmp(cmd, "i2cscan"))  v_i2cscan(argc, argv);
    else if (!strcmp(cmd, "load"))     v_load(argc, argv);
    else if (!strcmp(cmd, "meter"))    v_meter(argc, argv);
    else {
        for (int i = 0; i < s_ncmds; i++)
            if (!strcasecmp(cmd, s_cmds[i].name)) { s_cmds[i].fn(argc, argv); return; }
        // Unknown verb: REPLY, never silence — silence here masqueraded as "flasherase hangs"
        // and earlier as the phantom store bug.
        if (s_cmds_dropped > 0)
            rbbench_reply("unknown command: %s (try help; NOTE %d verb(s) were dropped at registration)",
                          cmd, s_cmds_dropped);
        else
            rbbench_reply("unknown command: %s (try help)", cmd);
    }
}

static void rx_task(void *arg) {
    (void)arg;
    static char buf[RBBENCH_LINE_MAX]; int len = 0;
    uint8_t c;
    esp_rom_printf("# rbbench rx_task up\n");   /* ROM path — proves the task scheduled even if the driver TX is mute */
    rbbench_reply("# rbbench ready");
    for (;;) {
        if (uart_read_bytes(RBBENCH_UART, &c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r') continue;
        if (c == '\n') { buf[len] = 0; dispatch(buf); len = 0; continue; }
        if (len < RBBENCH_LINE_MAX - 1) buf[len++] = (char)c; else len = 0;
    }
}

void rbbench_init(i2c_master_bus_handle_t sensor_bus) {
    s_sensor_bus = sensor_bus;
    if (!s_exec_mtx) s_exec_mtx = xSemaphoreCreateMutex();

    // Self-create the DUT/rbAmp sensor bus (25/26) if the app didn't provide one. The app
    // bus-handle is null whenever rbpower/rbamp_bridge init is gated off or fails (e.g. the
    // freq=0 path), which would leave i2cscan1/i2cget dead — making an empty scan look like
    // "no hardware" when it's really "bus never came up". Owning it here makes bus
    // diagnostics independent of app init. No double-init: only when null.
    // EXPLICIT ports (ESP32 has only 2 I2C controllers): sensor=port 0, load=port 1. Using
    // .i2c_port=-1 (auto) collided when the app's rbamp_bridge already held a bus — the load
    // bus auto-select aliased/failed, so load ops broke ONLY when rbAmp was present on bus 0
    // Explicit ports remove the ambiguity.
    if (s_sensor_bus == NULL) {
        i2c_master_bus_config_t sb = {
            .i2c_port = 0, .sda_io_num = SENSOR_SDA, .scl_io_num = SENSOR_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        esp_err_t se = i2c_new_master_bus(&sb, &s_sensor_bus);
        ESP_LOGW(TAG, "DUT sensor bus self-create (port0 SDA=%d SCL=%d): %s",
                 SENSOR_SDA, SENSOR_SCL, esp_err_to_name(se));
    }

    // Own the load bus (19/23) on the OTHER controller (port 1).
    i2c_master_bus_config_t lb = {
        .i2c_port = 1, .sda_io_num = LOAD_SDA, .scl_io_num = LOAD_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&lb, &s_load_bus) != ESP_OK)
        ESP_LOGW(TAG, "load bus init failed — `load` verb disabled");
    else { load_write(LOAD_ADDR_A, 0); load_write(LOAD_ADDR_B, 0); }  // fail-safe OFF

    // UART0 console (USB). RX via driver; logs (drop to WARN) share TX.
    // Canonical order: param_config -> set_pin -> driver_install. With CONSOLE_NONE the
    // IDF console no longer routes U0RXD for us, so we MUST set the pins explicitly
    // (U0TXD=GPIO1, U0RXD=GPIO3) or RX gets nothing.
    uart_config_t uc = { .baud_rate = 115200, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT };
    // If a UART0 driver is already installed (e.g. Arduino HardwareSerial Serial.begin() ran
    // first in a build that didn't gate it), our install would no-op with ESP_ERR_INVALID_STATE
    // and we'd be reading/writing through a foreign driver → dead console. Delete any existing
    // driver so rbbench is the sole UART0 owner. (Otherwise the console is dead in a Wi-Fi build.)
    uart_driver_delete(RBBENCH_UART);
    esp_err_t pc_rc = uart_param_config(RBBENCH_UART, &uc);
    esp_err_t sp_rc = uart_set_pin(RBBENCH_UART, 1, 3, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    esp_err_t di_rc = uart_driver_install(RBBENCH_UART, RBBENCH_UART_BUF, 0, 0, NULL, 0);
    // esp_rom_printf goes straight to the UART0 TX FIFO via ROM — visible even under
    // CONSOLE_NONE and even if the driver install failed (when uart_write_bytes is silent).
    esp_rom_printf("rbbench UART0: param_config=%d set_pin=%d driver_install=%d\n",
                   pc_rc, sp_rc, di_rc);

    // RS485 to DW-81 reference meter on UART2 (9600 8N2, auto-direction MAX485).
    uart_config_t mc = { .baud_rate = 9600, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_2,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT };
    if (uart_driver_install(RS485_UART, 256, 0, 0, NULL, 0) == ESP_OK) {
        uart_param_config(RS485_UART, &mc);
        uart_set_pin(RS485_UART, RS485_TX, RS485_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    } else {
        ESP_LOGW(TAG, "RS485/UART2 init failed — `meter` verb disabled");
    }

    xTaskCreate(rx_task, "rbbench", 4096, NULL, 5, NULL);
}
