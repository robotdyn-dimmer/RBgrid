/**
 * History-record timestamp semantics — block E.1 / spec §5.2 / principle U3.
 *
 * As of the October contract, history `ts` is wall-clock EPOCH seconds. But records written
 * by pre-October firmware (or before the device ever got SNTP) carry seconds-of-uptime as
 * `ts` and stay in flash until cleared. Those must render as "до синхронизации времени"
 * (grouped, no fake calendar date), NEVER as 1970.
 *
 * Heuristic (root's October banner): a plausible wall-clock epoch is ≥ ~1.6e9
 * (2020-09-13). Anything below is legacy/uptime or a pre-sync shelf (ts=0).
 */

/** Epoch-seconds floor below which a `ts` is treated as legacy/uptime, not wall-clock. */
export const WALLCLOCK_EPOCH_FLOOR = 1_600_000_000; // 2020-09-13T12:26:40Z

/** True when `ts` is a real wall-clock epoch (post-SNTP), false for uptime/legacy/ts=0. */
export function isWallClockTs(ts: number | undefined | null): boolean {
  return typeof ts === 'number' && ts >= WALLCLOCK_EPOCH_FLOOR;
}

/** True for a legacy/pre-sync record (uptime-style ts or the ts=0 shelf). */
export function isLegacyTs(ts: number | undefined | null): boolean {
  return !isWallClockTs(ts);
}

/** The ts=0 "shelf" bucket that the device accumulates into while time is invalid. */
export function isUnsyncedShelf(ts: number | undefined | null): boolean {
  return ts === 0;
}

/**
 * Partition records into dated (real epoch) and legacy (pre-sync) groups, preserving order.
 * Charts/tables render `dated` on a time axis and collect `legacy` under a
 * "до синхронизации времени" group with no calendar date (spec §5.2).
 */
export function partitionByTime<T extends { ts: number }>(records: T[]): { dated: T[]; legacy: T[] } {
  const dated: T[] = [];
  const legacy: T[] = [];
  for (const r of records) (isWallClockTs(r.ts) ? dated : legacy).push(r);
  return { dated, legacy };
}

/**
 * Interval semantics (root canon ed876014): a record's `ts` is the moment the bucket CLOSED =
 * the END of its interval `[ts − dur, ts]`. The hour 23:00–24:00 carries ts = 00:00 of the next
 * day, so filtering «сегодня» by `ts ≥ midnight` counted yesterday's last hour into today (.110:
 * 800 Wh «today» vs 668 Wh in the day counter). The webapp works in interval STARTS — what a
 * reader calls «час 16:00» — so `useHistory` rewrites `ts` to the start once, keeps the wire value
 * in `ts_end`, and every consumer (window sums, hour slots, labels, CSV) stays start-based.
 */
const PERIOD_SPAN: Record<string, number> = { hour: 3600, day: 86400, week: 7 * 86400, month: 28 * 86400, year: 365 * 86400 };

/** Interval start of a record (seconds). `dur` is capped by the period, so an over-long bucket
 *  (an anomalous 4500 s hour) or a closing a few seconds late never lands in a neighbour. */
export function intervalStart(r: { ts: number; dur?: number }, period: string): number {
  if (!isWallClockTs(r.ts)) return r.ts;
  const span = PERIOD_SPAN[period] ?? 3600;
  const dur = typeof r.dur === 'number' && r.dur > 0 ? Math.min(r.dur, span) : span;
  return r.ts - dur;
}

/** Records with `ts` = interval start (wire `ts` kept as `ts_end`); idempotent. */
export function toIntervalStarts<T extends { ts: number; dur?: number; ts_end?: number }>(records: T[], period: string): T[] {
  return records.map((r) => (r.ts_end !== undefined ? r : { ...r, ts: intervalStart(r, period), ts_end: r.ts }));
}

/** A wire query for the window [from, to) in interval starts: the records CLOSE in (from, to + one bucket]. */
export function wireRange(from: number | undefined, to: number | undefined, period: string): { from?: number; to?: number } {
  const span = PERIOD_SPAN[period] ?? 3600;
  return { ...(from !== undefined ? { from: from + 1 } : {}), ...(to !== undefined ? { to: to + span } : {}) };
}
