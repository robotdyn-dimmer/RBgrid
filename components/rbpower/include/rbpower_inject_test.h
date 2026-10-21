/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbpower_inject_test.h
 * @brief Functional self-test for the external-source measurement injection API
 *        (rbpower_home_inject_halfcycle / rbpower_home_inject_200ms).
 *
 * Self-contained (no Unity dependency); follows the project's inline-test convention.
 * Runs on the target and prints the UART summary line
 *   "RBTEST inject SUMMARY: <P> passed, <F> failed"
 * which test tooling parses for an automated PASS/FAIL verdict (pass = 0 failures).
 */
#pragma once

#include "rbhome_types.h"  /* rbpower_unit_id_t */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run the injection-API functional test suite.
 *
 * Requires the home network to be initialized and @p known_unit_id to be a unit
 * that is actually registered in the running configuration (so the valid-inject /
 * round-trip cases can resolve it). Call AFTER rbpower init.
 *
 * @param[in] known_unit_id  A real, registered unit id (e.g. one created by the app config).
 * @return number of FAILED checks (0 == all passed).
 */
int rbpower_home_inject_run_tests(rbpower_unit_id_t known_unit_id);

#ifdef __cplusplus
}
#endif
