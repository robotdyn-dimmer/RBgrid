// rbbench — development/test-rig debug console for RBgrid (generic half).
//
// A small UART0 line console exposing the BRIDGE_SHELL protocol (byte-compatible
// with a host-side Python test harness (bridge + BridgeSMBus
// shim). Generic verbs (ping/help/i2cget/i2cset/i2cwrite/i2cscan/load) plus a
// registration hook so RBgrid adds its own introspection verbs (store/extflash/
// ring/shutdown) without touching the parser.
//
// Compile only when CONFIG_RBGRID_DEBUG_BRIDGE=y (RBgrid owns that Kconfig flag
// + the CMakeLists wiring + the app_main init call).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// Verb handler. argv[0] = verb name. Return 0 on success (convention only).
// Use rbbench_reply() for all output so it lands on the console UART with CRLF.
typedef int (*rbbench_cmd_fn)(int argc, char **argv);

// Start the console. `sensor_bus` = RBgrid's existing i2c_master bus handle for
// the rbAmp sensor bus (GPIO25/26) — rbbench borrows it for i2cget/i2cset/
// i2cwrite/i2cscan (one master, no contention). rbbench creates and owns the
// separate load bus (GPIO21/22) internally for the `load` verb. Spawns the RX
// task. Call once, behind CONFIG_RBGRID_DEBUG_BRIDGE.
void rbbench_init(i2c_master_bus_handle_t sensor_bus);

// Register an RBgrid-specific verb. Call BEFORE or AFTER rbbench_init (registry
// is consulted at dispatch time). Names are matched case-insensitively. Up to
// RBBENCH_MAX_CMDS (16) custom verbs.
void rbbench_register_cmd(const char *name, rbbench_cmd_fn fn);

// Emit a response line on the console UART (CRLF appended). printf-style.
void rbbench_reply(const char *fmt, ...);

// Run one console line for a remote caller (HTTP) and collect its replies into `out` (lines joined
// with '\n', NUL-terminated). Replies made by OTHER tasks meanwhile still go to the UART. Serialized
// with the UART console. Returns the output length, -1 = console not initialized / bad args,
// -2 = another verb still running after 5 s. *truncated (optional) = output did not fit.
int rbbench_exec(const char *line, char *out, size_t cap, bool *truncated);

// Parse a uint token accepting "0x.." or decimal, reject sign/space. Returns
// true on success. Exposed for RBgrid verb handlers.
bool rbbench_parse_uint(const char *s, uint32_t *out, uint32_t max_val);

#ifdef __cplusplus
}
#endif
