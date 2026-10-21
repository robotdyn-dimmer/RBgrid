/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbpower_inject_test.c
 * @brief Functional self-test for the external-source measurement injection API
 *        (rbpower_home_inject_halfcycle / rbpower_home_inject_200ms).
 *
 * Self-contained mini-harness (no Unity dependency) matching the project's inline-test
 * style. Emits per-check PASS/FAIL logs and a single machine-parseable summary line:
 *     "RBTEST inject SUMMARY: <P> passed, <F> failed"
 * which test tooling parses for an automated verdict (0 failures == pass).
 */

#include "rbpower_inject_test.h"
#include "rbpower.h"        /* rbpower_err_t, RBPOWER_OK, RBPOWER_ERR_* */
#include "rbhome_types.h"   /* injection API, realtime structs, get_unit_200ms_data */

#include <string.h>
#include <math.h>
#include "esp_log.h"

static const char *TAG = "RBINJECT_TEST";

static int s_pass;
static int s_fail;

/** Record one check: count it as passed/failed and log PASS/FAIL with @p name. */
#define RBT_CHECK(cond, name)                                  \
    do {                                                       \
        if (cond) {                                            \
            s_pass++;                                          \
            ESP_LOGI(TAG, "PASS: %s", (name));                 \
        } else {                                               \
            s_fail++;                                          \
            ESP_LOGE(TAG, "FAIL: %s", (name));                 \
        }                                                      \
    } while (0)

int rbpower_home_inject_run_tests(rbpower_unit_id_t known_unit_id)
{
    s_pass = 0;
    s_fail = 0;

    ESP_LOGI(TAG, "=== rbpower_home_inject functional tests (unit 0x%04X) ===",
             (unsigned)known_unit_id);

    rbpower_unit_realtime_average_200ms_t m200;
    rbpower_unit_realtime_halfcycle_t mhc;
    memset(&m200, 0, sizeof(m200));
    memset(&mhc, 0, sizeof(mhc));

    /* 1. NULL data is rejected (checked before the init/lookup, so safe anytime). */
    RBT_CHECK(rbpower_home_inject_200ms(known_unit_id, NULL) == RBPOWER_ERR_INVALID_ARG,
              "inject_200ms(NULL) -> INVALID_ARG");
    RBT_CHECK(rbpower_home_inject_halfcycle(known_unit_id, NULL) == RBPOWER_ERR_INVALID_ARG,
              "inject_halfcycle(NULL) -> INVALID_ARG");

    /* 2. Unknown unit id is rejected. */
    RBT_CHECK(rbpower_home_inject_200ms((rbpower_unit_id_t)0xFFFF, &m200) == RBPOWER_ERR_INVALID_ARG,
              "inject_200ms(unknown unit) -> INVALID_ARG");
    RBT_CHECK(rbpower_home_inject_halfcycle((rbpower_unit_id_t)0xFFFF, &mhc) == RBPOWER_ERR_INVALID_ARG,
              "inject_halfcycle(unknown unit) -> INVALID_ARG");

    /* 3. A valid 200ms injection succeeds and round-trips through the public getter. */
    const float marker = 1234.5f;
    m200.instant_power_net_w = marker;
    m200.is_valid = true;
    RBT_CHECK(rbpower_home_inject_200ms(known_unit_id, &m200) == RBPOWER_OK,
              "inject_200ms(valid unit) -> OK");

    rbpower_unit_realtime_average_200ms_t readback;
    memset(&readback, 0, sizeof(readback));
    rbpower_err_t rg = rbpower_get_unit_200ms_data(known_unit_id, &readback);
    RBT_CHECK(rg == RBPOWER_OK, "get_unit_200ms_data(valid unit) -> OK");
    RBT_CHECK(fabsf(readback.instant_power_net_w - marker) < 0.01f,
              "injected 200ms value round-trips via getter");

    /* 4. A valid half-cycle injection is accepted. */
    RBT_CHECK(rbpower_home_inject_halfcycle(known_unit_id, &mhc) == RBPOWER_OK,
              "inject_halfcycle(valid unit) -> OK");

    ESP_LOGI(TAG, "RBTEST inject SUMMARY: %d passed, %d failed", s_pass, s_fail);
    return s_fail;
}
