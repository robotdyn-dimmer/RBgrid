/*
 * bench_kit — built-in load control + reference meter for running an app ON the
 * rbAmp bench ESP32 (one ESP32 on the bench; no external bridge, no console needed).
 *
 * Bench wiring (ESP32-D0WD-V3, 4 MB flash, NO PSRAM):
 *   relay bus  I2C SDA19 / SCL23 @100 kHz (behind ADuM1250), PCF8574 active-high, 0x00 = OFF
 *     0x27 loops   : 1x=0x01 (2x=0x06 3x=0x1A 4x=0x6A 5x=0xAA). Current flows only with a loop AND a load bit.
 *     0x25 fine    : 0x01 2.16 A · 0x02 1.12 A · 0x04 0.45 A · 0x08/0x10 0.22 A · 0x20/0x40 0.074 A (bits add)
 *     0x24 coarse  : 0x01..0x08 ~4.4 A each, 0x10..0x80 ~2.2 A each — HIGH POWER, off unless allow_coarse
 *     0x21 low-I   : mA cascade (not used by the presets)
 *     0x27 boots ALL-ON (PCF power-up state) -> bench_kit_init() writes 0x00 to every bank first.
 *   YP9830 3-phase reference: UART2 TX27 / RX32, RS-485 auto-direction, 19200 8N1, addr 1, FC03,
 *     Int32 ABCD: phase A U@6 I@8 P@10 PF@12 (U/100 I/1000 P/100 PF/1000), E@38 (/100 kWh), F@40 (/10).
 *   DUT / rbAmp bus: I2C SDA25 / SCL26 (the app's own bus). Strap pins: IO21 HIGH = I2C transport,
 *     IO4 HIGH = production (maintenance strap off), IO22 = DUT NRST (open-drain, never drive HIGH).
 *
 * Safety: loads switch ON loop-first/load-second and OFF load-first/loop-second; a dead-man timer
 * forces everything OFF after max_on_s; esp_restart() also switches everything OFF (shutdown hook).
 * A crash leaves the PCF latched until the next boot, which clears it — keep max_on_s short.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      relay_i2c_port;   /* default I2C_NUM_1 (the app keeps I2C_NUM_0 for rbAmp on 25/26) */
    uint32_t max_on_s;         /* dead-man: forced OFF after this many seconds ON (0 = 7200) */
    bool     allow_coarse;     /* permit 0x24 (>= 2.2 A per bit) — off by default */
    bool     yp_enable;        /* install UART2 for the YP9830 */
    bool     dut_pins;         /* drive IO21 HIGH, IO4 HIGH, release IO22 */
} bench_kit_config_t;

#define BENCH_KIT_CONFIG_DEFAULT() { .relay_i2c_port = 1, .max_on_s = 7200, .allow_coarse = false, \
                                     .yp_enable = true, .dut_pins = true }

/* Load presets at loop x1 (~222 V mains). Nominal power in the name. */
typedef enum {
    BK_OFF = 0,
    BK_P16,      /* 0x25=0x20  0.074 A  ~16 W  */
    BK_P50,      /* 0x25=0x08  0.22 A   ~50 W  */
    BK_P100,     /* 0x25=0x04  0.45 A   ~100 W */
    BK_P250,     /* 0x25=0x02  1.12 A   ~250 W (default bench load) */
    BK_P350,     /* 0x25=0x06  1.57 A   ~350 W */
    BK_P480,     /* 0x25=0x01  2.16 A   ~480 W (short dwell) */
    BK_PRESET_COUNT
} bench_kit_preset_t;

typedef struct {
    float U, I, P, PF;   /* phase A */
    float E_kwh;         /* meter's own energy register */
    float F;
} bench_kit_yp_t;

esp_err_t bench_kit_init(const bench_kit_config_t *cfg);

/* Raw bank write (0x21/0x24/0x25/0x27); 0x24 refused unless allow_coarse. */
esp_err_t bench_kit_relay(uint8_t addr, uint8_t value);
esp_err_t bench_kit_set_loop(uint8_t loop_mask);                 /* 0x27 */
esp_err_t bench_kit_set_load(uint8_t fine_0x25, uint8_t coarse_0x24); /* keeps the loop */
esp_err_t bench_kit_preset(bench_kit_preset_t p);                /* loop x1 + preset, or all OFF */
void      bench_kit_all_off(const char *why);
void      bench_kit_kick(void);                                  /* restart the dead-man window */
bool      bench_kit_load_on(void);

/* Step program: run in a background task; ends with all OFF. */
typedef struct { bench_kit_preset_t preset; uint32_t seconds; } bench_kit_step_t;
esp_err_t bench_kit_run_program(const bench_kit_step_t *steps, int n, bool repeat);
void      bench_kit_stop_program(void);

/* YP9830 */
esp_err_t bench_kit_yp_read(bench_kit_yp_t *out);
/* Background energy integrator (trapezoid on phase-A P, period_ms >= 250). */
esp_err_t bench_kit_yp_integrate_start(uint32_t period_ms);
void      bench_kit_yp_integrate_stop(void);
double    bench_kit_yp_wh(uint32_t *samples, uint32_t *fails);   /* Wh since start */

#ifdef __cplusplus
}
#endif
