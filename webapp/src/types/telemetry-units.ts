/**
 * Live telemetry per-unit shape — mirror of the firmware `units` section of the
 * /ws/telemetry stream. FINAL contract, finalized by the firmware team 2026-06-19
 * (rbAmp Phase 6), per the shape decisions in memory `telemetry-units-contract`.
 *
 * Canonical replacement for the divergent `RealtimeMeasurement` (descriptive names,
 * array form) — see @deprecated note there. The live-telemetry UI + useTelemetry
 * rewrite onto these types is the remaining consumer work.
 *
 * The `units` map is keyed by 4-digit HEX unit id ("9020", unified with history) plus
 * the special keys "v_bus" / "main" / "alt". rbAmp (I2C) and ESP-NOW units share this map.
 *
 * NOTE: the OUTER /ws/telemetry envelope (device_id / timestamp / sequence / system{})
 * is not yet confirmed by root — model it when building the telemetry UI.
 */

/** Physical TRANSPORT of a unit's data (stable since Phase 6). NOT the field-class — one
 *  transport (i2c) will carry multiple classes once STD modules land, so never gate on this. */
export type TelemetryUnitSrc = 'i2c' | 'espnow' | string;

/** Field CLASS of a unit = the schema.fields_by_source key. Canon root 2026-10-12. */
export type TelemetryFieldClass = 'rbamp' | 'espnow' | string;

/** Fallback transport→class map while `src_class` isn't on the wire yet (confirmed correct
 *  by root: today every i2c unit is rbAmp). `rbamp_std` arrives additively with STD modules. */
const SRC_TRANSPORT_TO_CLASS: Record<string, TelemetryFieldClass> = {
  i2c: 'rbamp',
  espnow: 'espnow',
};

/**
 * Resolve a unit's field-class (= its `fields_by_source` key) for dash-not-0.00 gating.
 * Prefers the explicit `src_class` (coming next firmware canon); else maps the transport `src`.
 */
export function resolveSrcClass(unit: Pick<TelemetryUnit, 'src_class' | 'src'>): TelemetryFieldClass {
  return unit.src_class ?? SRC_TRANSPORT_TO_CLASS[unit.src] ?? unit.src;
}

/** rbAmp energy/Wh provenance (i2c units only). 'on_device' = device-computed Wh (wall-clock L9). */
export type TelemetryEnergyMode = 'none' | 'on_device' | 'host';

/**
 * 3-second finalized averages, emitted alongside the live 200ms values only when a
 * 3s window completes (omitted between — cache last-known). Use for stable numeric
 * readouts/logs; use the unit's top-level live fields for gauges/needles.
 */
export interface TelemetryUnitResult {
  /** RMS voltage, V */
  v?: number;
  /** RMS current, A */
  i?: number;
  /** Active power, W */
  p_act?: number;
  /** Reactive power, VAR */
  p_rea?: number;
  /** Power factor */
  pf?: number;
}

/**
 * One phase / leg of a per-phase metering unit (STD-U3I3; root delta №4 «Телеметрия
 * per-phase», live on .139). `ph` = phase number 1..3 (explicit — a 2-of-3 unit carries e.g.
 * 1 and 3). UNKNOWN fields are OMITTED (never 0-filled). Short keys: p = active W, q = VAR.
 */
export interface TelemetryPhase {
  ph: number;
  /** RMS voltage L-N, V */
  v?: number;
  /** RMS current, A */
  i?: number;
  /** Active power, W (signed: export < 0) */
  p?: number;
  /** Reactive power, VAR */
  q?: number;
  /** Power factor */
  pf?: number;
  /** Grid-voltage phases on the bus unit (canon fc16621e): measuring rbAmp module index. */
  module?: number;
  /** …and its V-input index (diagnostic only). */
  vch?: number;
  /** Voltage phase angle vs L1, degrees (L1 = 0; root 9bc08b40, 3-phase STD only). */
  angle?: number;
}

/** Phase rotation measured by a 3-phase STD module (root 9bc08b40). */
export type PhaseSeq = 'forward' | 'reverse' | 'abnormal';

/**
 * One unit in the telemetry `units` map. Fields are mostly optional because special
 * units carry subsets (e.g. "v_bus" = {v, src}); regular/rbAmp units carry the full set.
 * Top-level electrical values are the LIVE 200ms reading.
 */
export interface TelemetryUnit {
  /** BASIC channel whose CT is fitted backwards — the load isn't metered (root d03f19ed). */
  reversed?: boolean;
  /** Current as % of the CT rating (root d03f19ed). */
  load_pct?: number;
  /** Near the CT's limit (root d03f19ed). */
  ct_warn?: boolean;
  /** The current is being clipped at the CT's limit — energy under-counted (root d03f19ed). */
  clip?: boolean;
  /**
   * Hardware presence: true = configured AND hardware detected (active);
   * false = configured but hardware absent (dormant — rbAmp probe failed). Not a network-online flag. Optional: special units may omit it.
   */
  present?: boolean;
  /** Live RMS voltage, V. OMITTED for current-only rbAmp units (read system voltage from "v_bus"). */
  v?: number;
  /** Live RMS current, A */
  i?: number;
  /** Live active power, W */
  p_act?: number;
  /** Live reactive power, VAR */
  p_rea?: number;
  /** Live power factor */
  pf?: number;
  /** Energy flow direction (1 = import, ...) */
  dir?: number;

  /** Cumulative energy imported */
  e_in?: number;
  /** Today's imported energy */
  e_day_in?: number;
  /** Cumulative energy exported */
  e_out?: number;
  /** Today's exported energy */
  e_day_out?: number;
  /** Today's cost (present on aggregate units like "main") */
  cost_day?: number;

  /** Physical transport (i2c | espnow). Always present. Do NOT gate fields on this. */
  src: TelemetryUnitSrc;
  /** Field class = the schema.fields_by_source key (rbamp | espnow). Additive,
   *  arrives next firmware canon; until then derive via resolveSrcClass(). Gate fields on THIS. */
  src_class?: TelemetryFieldClass;

  // ---- I2C-UID decomposition (i2c units) — decoded by firmware, NOT the frontend ----
  /** I2C bus index */
  bus?: number;
  /** rbAmp module index */
  module?: number;
  /** Channel within the module */
  ch?: number;
  /** The jack as printed on the module: «I0» / «L1» / «V» / «L1–L3» (root 16.10). */
  ch_label?: string;
  /** Wh provenance (i2c units, present when the module is live; else omitted) */
  energy_mode?: TelemetryEnergyMode;

  /** 3-second finalized averages (present on a completed window; cache between). */
  result?: TelemetryUnitResult;

  // ---- contract v4 additive (live ≥7e0b1301; units{} still keyed by short name, re-key by point is T8) ----
  /** Stable point slug for this unit ([a-z0-9_.-]{1,32}). Additive; outer key stays short-name until T8. */
  point?: string;
  /** Capability tags for this unit, e.g. ["DIRECTION","DEVICE_ENERGY","ENERGY_MWH","PERSISTED"]. */
  caps?: string[];
  /** Display resolution hint (decimal places). */
  res?: number;
  /** Live quality bitmask for this unit (contract §1; see core/quality). */
  q_live?: number;
  /** Seconds since the unit's last live sample (null = none since boot) — root, after 60b52a6. */
  age_s?: number | null;
  /** Power resolution of the point, W (root/rbamp-dev, Ф6): below it there is no flow. */
  p_res_w?: number;
  /**
   * Per-phase readings (U3I3 units only; root delta №4). Absent = no per-phase data — normal,
   * not an error: the firmware drops a snapshot older than 10 s, so never cache it past the
   * frame. Top-level v/i/p of such a unit are Σ (p, q, i) / mean L-N (v).
   */
  ph?: TelemetryPhase[];
  /** Phase rotation (3-phase STD only; absent = not measured). */
  phase_seq?: PhaseSeq;
  /**
   * Bus unit only (canon fc16621e): number of fresh measured grid phases (1..3). `v` is then the
   * mean L-N over them, and `ph[]` = [{ph, v, module, vch}] when >1 phase or the phase ≠ L1.
   */
  n_ph?: number;

  // ---- frame v2 (root canon 274a569c, `?v=2`): every unit keyed by HEX4 UPPERCASE ----
  /** HEX4 id (v2; also on the former "main"/"v_bus" objects). */
  unit_id?: string;
  /** units-config type: CURRENT_LOAD | VOLTAGE_BUS | … (v2). */
  type?: string;
  /** units-config role (v2): MAIN_SUPPLY | ALTERNATIVE_SUPPLY | DEVICE_LOAD | … */
  role?: string;
}

/**
 * The telemetry `units` section: a map from unit key to its current telemetry.
 * - v1 (current live): keys are hex id ("9020") plus special "v_bus" / "main" / "alt".
 * - v2 (T12 flip): keys become UNIFORM 4-hex-lowercase unit_id with no 0x — including the
 *   former "main" → "0006". `point` stays an attribute (re-key by point was CANCELLED, root
 *   accepted webapp's recommendation 2026-10-12). unit_id also stays inside each object.
 */
export type TelemetryUnitsMap = Record<string, TelemetryUnit>;

/**
 * Full live telemetry frame — the outer envelope of the /ws-telemetry push and the
 * GET /api/telemetry/latest snapshot (same compact shape, confirmed on the wire by
 * standfw 2026-06-19). Flat, NOT a BaseResponse.
 *
 * NOTE: only `net_pwr`/`net_dir` are system-level here; a full system-totals block
 * (sum power/energy/cost across units) is a firmware gap (flagged to root) — aggregate
 * client-side or consume root's future top-level totals block.
 */
/**
 * Device-computed system totals (the `system{}` block of the telemetry frame).
 * Keys confirmed from the wire on the real device (192.168.1.110) 2026-06-20.
 */
export interface TelemetrySystemTotals {
  /** The hub's last boot (root canon 33acbedd): unix ts (0 = clocks were invalid), reason
   *  («power-on» / «planned restart» / «task watchdog» / «PANIC» …), planned, downtime_s (0 = unknown). */
  boot?: { ts?: number; reason?: string; planned?: boolean; downtime_s?: number };
  /** Standby floor computed on the hub (root, next canon): median of the 01–05 h hourly means over
   *  7 nights, W; with the number of nights it covers. */
  base_w?: number;
  base_w_nights?: number;
  /** Total active power, W */
  p_act?: number;
  /** Total reactive power, VAR */
  p_rea?: number;
  /** Cumulative / daily energy imported, Wh */
  e_in?: number;
  e_day_in?: number;
  /** Cumulative / daily energy exported, Wh */
  e_out?: number;
  e_day_out?: number;
  /** Cumulative / daily cost */
  cost_total?: number;
  cost_day?: number;
  /** Cumulative / daily revenue (export) */
  rev_total?: number;
  rev_day?: number;
  /** Configured units count / present count */
  units?: number;
  present?: number;
  /** Live tariff state (E1, canon fc0f962b; full profile only). */
  tariff?: TelemetryTariff;
}

/**
 * Live tariff in the frame (`system.tariff`, root E1). Rates in the active plan's currency per
 * kWh; `buy_rate`/`sell_rate` 0 = no active plan. `cost_rate_h` = money/hour on NET power,
 * signed by direction (negative = earning). Per-line cost = line p_act × buy_rate (no per-line fields).
 */
export interface TelemetryTariff {
  zone?: string;
  buy_rate?: number;
  sell_rate?: number;
  cost_rate_h?: number;
  /** (zone rate + per-kWh levies) × (1 + VAT) — the basis of cost_day and the records (root,
   *  16.10); cost_rate_h is on it too when this is present. */
  buy_rate_all_in?: number;
  currency?: string;
  /** The factory example plan is in place, not the user's own (root 86f06270, W191-06). */
  placeholder?: boolean;
}

export interface TelemetryFrame {
  /**
   * Frame schema version: 1 (legacy supply keys "main"/"alt"/"v_bus") or 2 (canon 274a569c,
   * requested per call with `?v=2`: hex keys only + `type`/`role` per unit + the refs below).
   */
  v?: number;
  /** v2: HEX4 of the main input / alternative supply / voltage bus (absent = none). */
  main_unit?: string;
  alt_unit?: string;
  vbus_unit?: string;
  /** Local device timestamp */
  ts: number;
  /** SNTP/NTP timestamp — numeric epoch seconds (confirmed canon, root 2026-10-12). */
  sntp: number;
  /** Line frequency, Hz */
  freq: number;
  /** Where `freq` comes from (canon 2b3dc1fd): "measured" = a module/bus reported it within
   *  10 s; "nominal" = the configured nominal (no voltage source) — not a measurement. */
  freq_src?: 'measured' | 'nominal';
  /** Net system flow direction */
  net_dir: number;
  /** Net system power, W */
  net_pwr: number;
  /** Device-computed system totals (power/energy/cost across units). */
  system?: TelemetrySystemTotals;

  // ---- contract v4 additive, live ≥7e0b1301 (confirmed on hub .139 2026-10-12) ----
  /** Monotonic frame sequence (u32) since boot; a gap ⇒ dropped frames ("data delayed"). */
  seq?: number;
  /** Random u32 set at boot; a change ⇒ the hub rebooted (= ESP-NOW epoch). */
  boot_id?: number;
  /** Seconds since boot. */
  uptime_s?: number;
  /** Time quality: 0 unsynced · 1 synced · 2 holdover (see core/quality classifyTimeQuality). */
  tq?: number;
  /** Clock source: "sntp" | "rtc" | "manual" | "none". */
  time_src?: string;
  /** Seconds since the last successful time sync. */
  sync_age_s?: number;
  /** Active-alarm count + max severity (zeros until phase 1). */
  alarm_count?: number;
  alarm_max_sev?: number;

  /** Per-unit telemetry, keyed by hex unit id / "v_bus" / "main" / "alt" (by `point` from T8). */
  units: TelemetryUnitsMap;
}
