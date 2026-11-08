/**
 * Statistics history types — mirror of firmware GET /api/v1/history
 * (and MQTT `cmd:"history"`). FINAL contract, HW-validated 2026-06-19 by the firmware team.
 *
 * IMPORTANT: history responses are FLAT (`status`/`request`/`meta`/`data`), NOT the
 * BaseResponse envelope used by config endpoints. Parse on a dedicated path
 * (RBGridApiClient.getHistory), NOT through formatApiResponse().
 *
 * Replaces the divergent, speculative STATISTICS.* endpoints + HourlyStats/DailyStats/
 * MonthlyStats types (those are @deprecated and slated for removal).
 */

/** Aggregation bucket granularity. Each record = one aggregated period bucket. */
export type HistoryPeriod = 'hour' | 'day' | 'week' | 'month' | 'year';

/** Metric family selecting which record shape `data[]` carries. */
// `quality` and `direction` were dropped from /api/v1/history (firmware shrink, 2026-06-21):
// those metrics (THD events / ZC direction-phase) are ADC+ZC-only and never filled on the
// rbAmp fleet. THD itself stays available as thd_voltage/thd_current on the `electrical` record.
export type HistoryType = 'electrical' | 'energy' | 'economics';

/** Query parameters for a history request (HTTP query / MQTT command fields). */
export interface HistoryRequestParams {
  /** Unit id, hex string; "0x" prefix optional (e.g. "9020" or "0x9020"). */
  unit_id: string;
  period: HistoryPeriod;
  type: HistoryType;
  /** Page offset; 0 = newest record (results are newest-first). Default 0. */
  offset?: number;
  /** Page size, 1..720. Default 720. */
  limit?: number;
  /** Window start (unix seconds) — time-range query (firmware-landed). */
  from?: number;
  /** Window end (unix seconds). */
  to?: number;
  /**
   * Incremental-sync cursor (T9, live ≥7e0b1301): return strictly `ts > since`, ASC order,
   * stable across the ring wrap. Client keeps `next_since` and tops up. The ts=0 shelf is
   * NEVER included in a since-stream. Mutually the newer paging path (vs offset).
   */
  since?: number;
  /** Include the ts=0 "before time sync" shelf (default 0). Only honored outside since-mode. */
  include_unsynced?: 0 | 1;
}

/**
 * One ELECTRICAL history record = one aggregated period bucket.
 * Fixed-point is already decoded server-side. The `*_min`/`*_max` fields are the
 * per-bucket envelope around the avg fields, with `min <= avg <= max` guaranteed.
 */
export interface HistoryElectricalRecord extends HistoryRecordV4Header {
  /** Bucket timestamp, unix seconds */
  ts: number;
  /** RMS voltage (avg), V */
  v: number;
  /** RMS current (avg), A */
  i: number;
  /** Active power (avg), W. Key is `p_act` (unified with telemetry, was `p`). */
  p_act: number;
  /**
   * ⚠ schema_ver:3 ONLY — reactive power (avg), VAR. In v4 this key is REPURPOSED as the
   * quality bitmask (HistoryRecordV4Header.q) and reactive moves to `p_rea`. Branch on schema_ver.
   */
  q: number;
  /** schema_ver:3 ONLY — apparent power (avg), VA. In v4 renamed to `p_app`. */
  s: number;
  /** v4 reactive power (avg), VAR. Absent on rbAmp sources (see fields_by_source). */
  p_rea?: number;
  /** v4 apparent power (avg), VA. Absent on rbAmp sources. */
  p_app?: number;
  /** Power factor */
  pf: number;
  /** Frequency, Hz */
  f: number;
  /** Voltage THD, % */
  thd_v: number;
  /** Current THD, % */
  thd_i: number;
  /** Voltage envelope (min/max over the bucket), V */
  v_min: number;
  v_max: number;
  /** Current envelope (min/max over the bucket), A */
  i_min: number;
  i_max: number;
  /** Active-power envelope (min/max over the bucket), W */
  p_min: number;
  p_max: number;
}

/**
 * Common v4 record header fields (contract §2), additive on every record family. Present
 * from schema_ver:4; absent on the current live schema_ver:3 records (hence optional).
 */
export interface HistoryRecordV4Header {
  /** Covered duration, seconds (< nominal ⇒ partial period ⇒ q.PARTIAL). */
  dur?: number;
  /** Quality bitmask (u16, contract §1). See core/quality. NB: on v3 ELECTRICAL records the
   *  key `q` instead meant reactive power (VAR); v4 renames reactive to `p_rea`. Branch on schema_ver. */
  q?: number;
  /** Decimal resolution hint for display precision. */
  res?: number;
  /** Webapp-side: the wire `ts` (interval END) after useHistory rewrote `ts` to the interval start. */
  ts_end?: number;
}

/** ENERGY history record (type=energy). Wh on the wire in v3 and v4 (canon fa34d5d3); v4 adds the et_* totalizers. */
export interface HistoryEnergyRecord extends HistoryRecordV4Header {
  ts: number;
  /** Energy consumed, Wh. */
  ec: number;
  /** Energy supplied, Wh. */
  es: number;
  /** Net energy = ec−es (derived; always serialized). */
  en: number;
  /** v4 lifetime import totalizer at the bucket end, Wh (absent on MIGRATED records, q & 256). */
  et_in?: number;
  /** v4 lifetime export totalizer at the bucket end, Wh. */
  et_out?: number;
}

/** ECONOMICS history record (type=economics). Money in MAJOR units (the hub stores cents and
 *  divides by 100 on the wire — rbjson_serialize_economics): cost / revenue / net. */
export interface HistoryEconomicsRecord extends HistoryRecordV4Header {
  ts: number;
  /** Cost of consumed energy, major currency units (stored as cents on the hub). */
  c: number;
  /** Revenue from supplied energy. */
  r: number;
  /** Net cost = c−r (derived). */
  n: number;
  /** v4: ISO currency code. */
  cur?: string;
  /** v4: tariff plan id + version in force for this bucket. */
  plan_id?: number;
  plan_ver?: number;
}

/**
 * Record by metric family. electrical/energy/economics confirmed by root (2026-06-19/20).
 * quality/direction were removed from the firmware history API (2026-06-21) — never filled
 * on the rbAmp fleet; THD lives on the electrical record (thd_voltage/thd_current).
 */
export type HistoryRecord =
  | HistoryElectricalRecord
  | HistoryEnergyRecord
  | HistoryEconomicsRecord;

/** type=all combined response: the three families in one payload (root 2026-06-23). */
export interface HistoryAll {
  electrical: HistoryElectricalRecord[];
  energy: HistoryEnergyRecord[];
  economics: HistoryEconomicsRecord[];
}

/** Echo of the request as the device parsed it (device upper-cases the enums). */
export interface HistoryRequestEcho {
  unit_id: string;
  period: string;
  type: string;
  offset: number;
  limit: number;
}

/** Pagination metadata. `total_available` = REAL stored count, NOT ring capacity. */
export interface HistoryMeta {
  total_available: number;
  returned: number;
  has_more: boolean;
}

/**
 * Flat history response (HTTP /api/v1/history and MQTT `cmd:"history"`).
 * Generic over the record family. NOT a BaseResponse.
 */
export interface HistoryResponse<T = HistoryRecord> {
  status: 'ok' | 'error';
  request: HistoryRequestEcho;
  meta: HistoryMeta;
  /** Records, NEWEST-FIRST (`data[0]` = freshest). Reverse for an ascending x-axis. */
  data: T[];
}

export type ElectricalHistoryResponse = HistoryResponse<HistoryElectricalRecord>;

/**
 * v4 / since-cursor history response (T9, live ≥7e0b1301). A different flat shape from the
 * legacy {status,request,meta,data}: the device echoes unit/point/period/type + `schema_ver`
 * and an incremental cursor. `schema_ver` is 3 while record forms are still v3, 4 after T12.
 * Records are ASC (oldest-first) in since-mode. Hold `next_since` and top up while `has_more`.
 */
export interface HistorySinceResponse<T = HistoryRecord> {
  unit: string;
  point?: string;
  period: string;
  type: string;
  schema_ver: number;
  data: T[];
  /** Cursor for the next incremental request (pass as `since`). */
  next_since: number;
  has_more: boolean;
  /** Real stored count available for this (unit, period, type). */
  total_available: number;
}
