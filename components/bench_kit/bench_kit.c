/* bench_kit — see bench_kit.h */
#include "bench_kit.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "bench_kit";

#define RELAY_SDA 19
#define RELAY_SCL 23
#define RELAY_HZ  100000
#define YP_UART   UART_NUM_2
#define YP_TX     27
#define YP_RX     32
#define YP_ADDR   1
#define DUT_STRAP 21
#define DUT_MODE  4
#define DUT_NRST  22

enum { B21, B24, B25, B27, NB };
static const uint8_t k_addr[NB] = { 0x21, 0x24, 0x25, 0x27 };
static const uint8_t k_fine[BK_PRESET_COUNT] = { 0x00, 0x20, 0x08, 0x04, 0x02, 0x06, 0x01 };

static bench_kit_config_t s_cfg;
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev[NB];
static uint8_t s_port[NB];
static SemaphoreHandle_t s_mx;
static esp_timer_handle_t s_deadman;
static TaskHandle_t s_prog;
static volatile bool s_prog_stop;

static int bank_ix(uint8_t addr)
{
    for (int i = 0; i < NB; i++) if (k_addr[i] == addr) return i;
    return -1;
}

static esp_err_t write_bank(int i, uint8_t v)
{
    if (!s_dev[i]) return ESP_ERR_NOT_FOUND;
    esp_err_t e = i2c_master_transmit(s_dev[i], &v, 1, 50);
    if (e == ESP_OK) s_port[i] = v;
    return e;
}

bool bench_kit_load_on(void)
{
    return s_port[B27] && (s_port[B21] || s_port[B24] || s_port[B25]);
}

static void arm_deadman(void)
{
    esp_timer_stop(s_deadman);
    if (bench_kit_load_on()) esp_timer_start_once(s_deadman, (uint64_t)s_cfg.max_on_s * 1000000ULL);
}

void bench_kit_all_off(const char *why)
{
    if (!s_mx) return;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    /* load banks first, loops last */
    write_bank(B24, 0); write_bank(B25, 0); write_bank(B21, 0); write_bank(B27, 0);
    esp_timer_stop(s_deadman);
    xSemaphoreGive(s_mx);
    if (why) ESP_LOGW(TAG, "all relay banks OFF (%s)", why);
}

static void deadman_cb(void *arg) { bench_kit_all_off("dead-man timeout"); }
static void shutdown_hook(void) { bench_kit_all_off("restart"); }

void bench_kit_kick(void)
{
    if (!s_mx) return;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    arm_deadman();
    xSemaphoreGive(s_mx);
}

esp_err_t bench_kit_relay(uint8_t addr, uint8_t value)
{
    const int i = bank_ix(addr);
    if (i < 0) return ESP_ERR_INVALID_ARG;
    if (i == B24 && value && !s_cfg.allow_coarse) return ESP_ERR_NOT_ALLOWED;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    esp_err_t e = write_bank(i, value);
    arm_deadman();
    xSemaphoreGive(s_mx);
    return e;
}

esp_err_t bench_kit_set_loop(uint8_t loop_mask)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    esp_err_t e = ESP_OK;
    /* never switch a loop relay under load: drop the load, change the loop, restore */
    const uint8_t f = s_port[B25], c = s_port[B24], l = s_port[B21];
    if ((f || c || l) && loop_mask != s_port[B27]) { write_bank(B24, 0); write_bank(B25, 0); write_bank(B21, 0); }
    e = write_bank(B27, loop_mask);
    if (e == ESP_OK && loop_mask) { write_bank(B21, l); write_bank(B25, f); if (c) write_bank(B24, c); }
    arm_deadman();
    xSemaphoreGive(s_mx);
    return e;
}

esp_err_t bench_kit_set_load(uint8_t fine_0x25, uint8_t coarse_0x24)
{
    if (coarse_0x24 && !s_cfg.allow_coarse) return ESP_ERR_NOT_ALLOWED;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    esp_err_t e = write_bank(B25, fine_0x25);
    if (e == ESP_OK) e = write_bank(B24, coarse_0x24);
    arm_deadman();
    xSemaphoreGive(s_mx);
    return e;
}

esp_err_t bench_kit_preset(bench_kit_preset_t p)
{
    if (p <= BK_OFF || p >= BK_PRESET_COUNT) { bench_kit_all_off(NULL); return ESP_OK; }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    esp_err_t e = ESP_OK;
    if (s_port[B27] != 0x01) {                 /* loop first, with no load */
        write_bank(B24, 0); write_bank(B25, 0); write_bank(B21, 0);
        e = write_bank(B27, 0x01);
    }
    if (e == ESP_OK) { write_bank(B24, 0); write_bank(B21, 0); e = write_bank(B25, k_fine[p]); }
    arm_deadman();
    xSemaphoreGive(s_mx);
    ESP_LOGI(TAG, "preset %d: loop 0x01, 0x25=0x%02X (%s)", p, k_fine[p], esp_err_to_name(e));
    return e;
}

/* ---- step program ---- */

typedef struct { bench_kit_step_t *steps; int n; bool repeat; } prog_t;

static void prog_task(void *arg)
{
    prog_t *pg = arg;
    do {
        for (int i = 0; i < pg->n && !s_prog_stop; i++) {
            bench_kit_preset(pg->steps[i].preset);
            for (uint32_t t = 0; t < pg->steps[i].seconds * 10 && !s_prog_stop; t++) vTaskDelay(pdMS_TO_TICKS(100));
        }
    } while (pg->repeat && !s_prog_stop);
    bench_kit_all_off("program end");
    free(pg->steps); free(pg);
    s_prog = NULL;
    vTaskDelete(NULL);
}

esp_err_t bench_kit_run_program(const bench_kit_step_t *steps, int n, bool repeat)
{
    if (s_prog) return ESP_ERR_INVALID_STATE;
    if (!steps || n <= 0) return ESP_ERR_INVALID_ARG;
    prog_t *pg = calloc(1, sizeof *pg);
    if (!pg || !(pg->steps = malloc(n * sizeof *steps))) { free(pg); return ESP_ERR_NO_MEM; }
    memcpy(pg->steps, steps, n * sizeof *steps);
    pg->n = n; pg->repeat = repeat;
    s_prog_stop = false;
    if (xTaskCreate(prog_task, "bk_prog", 3072, pg, 3, &s_prog) != pdPASS) { free(pg->steps); free(pg); return ESP_ERR_NO_MEM; }
    return ESP_OK;
}

void bench_kit_stop_program(void)
{
    s_prog_stop = true;
    for (int i = 0; i < 50 && s_prog; i++) vTaskDelay(pdMS_TO_TICKS(20));
    bench_kit_all_off(NULL);
}

/* ---- YP9830 ---- */

static SemaphoreHandle_t s_yp_mx;
static bool s_yp_ok;

static uint16_t crc16(const uint8_t *b, int n)
{
    uint16_t c = 0xFFFF;
    for (int i = 0; i < n; i++) {
        c ^= b[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1;
    }
    return c;
}

static esp_err_t yp_regs(uint16_t start, uint16_t count, uint16_t *out)
{
    if (!s_yp_ok) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_yp_mx, portMAX_DELAY);
    uint8_t req[8] = { YP_ADDR, 0x03, start >> 8, start & 0xFF, count >> 8, count & 0xFF };
    const uint16_t c = crc16(req, 6);
    req[6] = c & 0xFF; req[7] = c >> 8;
    uint8_t buf[128];
    int total = 0;
    const int need = 3 + 2 * count + 2;
    esp_err_t rc = ESP_ERR_TIMEOUT;
    uart_flush_input(YP_UART);
    uart_write_bytes(YP_UART, (const char *)req, 8);
    uart_wait_tx_done(YP_UART, pdMS_TO_TICKS(50));
    const int64_t deadline = esp_timer_get_time() + 200000;
    while (esp_timer_get_time() < deadline && total < (int)sizeof buf && rc == ESP_ERR_TIMEOUT) {
        const int n = uart_read_bytes(YP_UART, buf + total, sizeof buf - total, pdMS_TO_TICKS(5));
        if (n <= 0) continue;
        total += n;
        for (int i = 0; i + need <= total; i++) {          /* tolerate echo / leading noise */
            if (buf[i] != YP_ADDR || buf[i + 1] != 0x03 || buf[i + 2] != 2 * count) continue;
            if (crc16(buf + i, need - 2) != ((uint16_t)buf[i + need - 1] << 8 | buf[i + need - 2])) continue;
            for (int k = 0; k < count; k++) out[k] = (uint16_t)buf[i + 3 + 2 * k] << 8 | buf[i + 4 + 2 * k];
            rc = ESP_OK;
            break;
        }
    }
    xSemaphoreGive(s_yp_mx);
    return rc;
}

static int32_t i32(const uint16_t *r, int k) { return (int32_t)((uint32_t)r[k] << 16 | r[k + 1]); }

esp_err_t bench_kit_yp_read(bench_kit_yp_t *m)
{
    uint16_t r[36];                                   /* regs 6..41 */
    esp_err_t e = yp_regs(6, 36, r);
    if (e != ESP_OK) return e;
    m->U = i32(r, 0) / 100.0f;
    m->I = i32(r, 2) / 1000.0f;
    m->P = i32(r, 4) / 100.0f;
    m->PF = i32(r, 6) / 1000.0f;
    m->E_kwh = i32(r, 32) / 100.0f;
    m->F = i32(r, 34) / 10.0f;
    return ESP_OK;
}

static TaskHandle_t s_int;
static volatile bool s_int_on;
static uint32_t s_int_ms, s_int_n, s_int_fail;
static double s_wh;

static void int_task(void *arg)
{
    float last_p = 0;
    int64_t last_t = 0;
    while (s_int_on) {
        bench_kit_yp_t m;
        const int64_t t = esp_timer_get_time();
        if (bench_kit_yp_read(&m) == ESP_OK) {
            if (last_t) s_wh += (last_p + m.P) * 0.5 * (double)(t - last_t) / 3.6e9;
            last_p = m.P; last_t = t; s_int_n++;
        } else {
            s_int_fail++;
        }
        vTaskDelay(pdMS_TO_TICKS(s_int_ms));
    }
    s_int = NULL;
    vTaskDelete(NULL);
}

esp_err_t bench_kit_yp_integrate_start(uint32_t period_ms)
{
    if (!s_yp_ok) return ESP_ERR_INVALID_STATE;
    if (s_int) return ESP_ERR_INVALID_STATE;
    s_int_ms = period_ms < 250 ? 250 : period_ms;
    s_wh = 0; s_int_n = s_int_fail = 0;
    s_int_on = true;
    return xTaskCreate(int_task, "bk_ypint", 3072, NULL, 4, &s_int) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void bench_kit_yp_integrate_stop(void) { s_int_on = false; }

double bench_kit_yp_wh(uint32_t *samples, uint32_t *fails)
{
    if (samples) *samples = s_int_n;
    if (fails) *fails = s_int_fail;
    return s_wh;
}

/* ---- init ---- */

esp_err_t bench_kit_init(const bench_kit_config_t *cfg)
{
    if (s_mx) return ESP_ERR_INVALID_STATE;
    s_cfg = cfg ? *cfg : (bench_kit_config_t)BENCH_KIT_CONFIG_DEFAULT();
    if (!s_cfg.max_on_s) s_cfg.max_on_s = 7200;
    s_mx = xSemaphoreCreateMutex();
    const esp_timer_create_args_t ta = { .callback = deadman_cb, .name = "bk_deadman" };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_deadman));

    const i2c_master_bus_config_t bc = {
        .i2c_port = s_cfg.relay_i2c_port, .sda_io_num = RELAY_SDA, .scl_io_num = RELAY_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    esp_err_t e = i2c_new_master_bus(&bc, &s_bus);
    if (e != ESP_OK) { ESP_LOGE(TAG, "relay bus: %s", esp_err_to_name(e)); return e; }
    int found = 0;
    for (int i = 0; i < NB; i++) {
        if (i2c_master_probe(s_bus, k_addr[i], 50) != ESP_OK) continue;
        const i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = k_addr[i], .scl_speed_hz = RELAY_HZ };
        if (i2c_master_bus_add_device(s_bus, &dc, &s_dev[i]) == ESP_OK) found++;
    }
    bench_kit_all_off(NULL);                    /* 0x27 powers up ALL-ON */
    esp_register_shutdown_handler(shutdown_hook);
    ESP_LOGI(TAG, "relay banks found %d/%d (0x21 %s 0x24 %s 0x25 %s 0x27 %s), all OFF, dead-man %" PRIu32 " s", found, NB,
             s_dev[B21] ? "ok" : "-", s_dev[B24] ? "ok" : "-", s_dev[B25] ? "ok" : "-", s_dev[B27] ? "ok" : "-", s_cfg.max_on_s);

    if (s_cfg.dut_pins) {
        gpio_config_t o = { .pin_bit_mask = (1ULL << DUT_STRAP) | (1ULL << DUT_MODE), .mode = GPIO_MODE_OUTPUT };
        gpio_config(&o);
        gpio_set_level(DUT_STRAP, 1);
        gpio_set_level(DUT_MODE, 1);
        gpio_config_t r = { .pin_bit_mask = 1ULL << DUT_NRST, .mode = GPIO_MODE_INPUT_OUTPUT_OD };
        gpio_config(&r);
        gpio_set_level(DUT_NRST, 1);            /* released (high-Z) */
    }
    if (s_cfg.yp_enable) {
        s_yp_mx = xSemaphoreCreateMutex();
        const uart_config_t uc = { .baud_rate = 19200, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                                   .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT };
        if (uart_driver_install(YP_UART, 512, 0, 0, NULL, 0) == ESP_OK && uart_param_config(YP_UART, &uc) == ESP_OK &&
            uart_set_pin(YP_UART, YP_TX, YP_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) == ESP_OK)
            s_yp_ok = true;
        bench_kit_yp_t m;
        if (s_yp_ok && bench_kit_yp_read(&m) == ESP_OK)
            ESP_LOGI(TAG, "YP9830: U=%.2f I=%.3f P=%.2f F=%.1f", m.U, m.I, m.P, m.F);
        else
            ESP_LOGW(TAG, "YP9830 not answering (UART2 TX27/RX32)");
    }
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}
