/* SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
/**
 * @file rbpower_logic_test.h
 * @brief On-device regression suite for the deterministic billing and statistics logic.
 *
 * Self-contained mini-harness (no Unity dependency), matching the project's inline-test
 * convention (see rbpower_inject_test.c). Runs PURE, deterministic functions with known
 * inputs and asserts exact outputs — no globals, no HW, no timing. Emits a single
 * machine-parseable summary line per suite that test tooling parses for an automated verdict:
 *     "RBTEST logic SUMMARY: <P> passed, <F> failed"   (0 failures == pass)
 *
 * Invoked at runtime via the `selftest` console verb (no rebuild needed).
 */
#ifndef RBPOWER_LOGIC_TEST_H
#define RBPOWER_LOGIC_TEST_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run the deterministic logic regression suite.
 * @return number of FAILED checks (0 == all passed).
 */
int rbpower_logic_run_tests(void);

#ifdef __cplusplus
}
#endif

#endif /* RBPOWER_LOGIC_TEST_H */
