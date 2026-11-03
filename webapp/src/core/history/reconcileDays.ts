/**
 * Day records checked against the hub's own hour records (rerun 16.10 RES-R01, sev 4: on .110
 * the day 15.10 closed with 0 while its hours held 5,8 kWh, so «Этот месяц» came out smaller
 * than one «Вчера»). For every day the hour archive still covers:
 *  - a day record that holds LESS than the sum of its hours takes the hours' sum;
 *  - a day with hours but no day record gets one from the hours.
 * Such a day is marked `reconciled` (shown as «по часовым записям»). Days older than the hour
 * archive stay as the hub wrote them. Records are interval starts (see core/time/recordTime).
 */
import type { HistoryRecord } from '../../types/history';

const SUM_KEYS = ['ec', 'es', 'en', 'c', 'r', 'n'] as const;
const DAY = 86400;

const num = (r: HistoryRecord, k: string): number => {
  const v = (r as unknown as Record<string, unknown>)[k];
  return typeof v === 'number' && Number.isFinite(v) ? v : 0;
};

export type Reconciled = HistoryRecord & { reconciled?: boolean };

export function reconcileDays(days: HistoryRecord[], hours: HistoryRecord[], dayStart: (ts: number) => number, now = Math.floor(Date.now() / 1000)): Reconciled[] {
  // Σ of the closed hours per local day (keyed by the day's start).
  const byDay = new Map<number, { sums: Record<string, number>; dur: number; n: number }>();
  for (const h of hours) {
    if (h.ts < 1_600_000_000) continue;
    const d = dayStart(h.ts);
    const e = byDay.get(d) ?? { sums: {}, dur: 0, n: 0 };
    for (const k of SUM_KEYS) e.sums[k] = (e.sums[k] ?? 0) + num(h, k);
    e.dur += num(h, 'dur') || 3600;
    e.n += 1;
    byDay.set(d, e);
  }
  const today = dayStart(now);
  const lead = (r: { [k: string]: number }) => Math.max(r.ec ?? 0, r.c ?? 0, r.es ?? 0);
  // A day may come as SEVERAL records (a restart split it: .110 15.10 = a 3-h fragment + a 0) —
  // compared and replaced as a whole, never each fragment topped up (that counted the day twice).
  const groups = new Map<number, HistoryRecord[]>();
  const out: Reconciled[] = [];
  for (const r of days) {
    if (r.ts < 1_600_000_000) { out.push(r); continue; }
    const d = dayStart(r.ts);
    const g = groups.get(d) ?? [];
    g.push(r);
    groups.set(d, g);
  }
  const seen = new Set<number>();
  groups.forEach((recs0, d) => {
    seen.add(d);
    // Fragments of a day cover different parts of it; records whose durations add up to MORE than
    // a day overlap (clock jumps on a test bench wrote the same day again) — keep the fullest one
    // instead of summing copies.
    const durSum = recs0.reduce((s, r) => s + (num(r, 'dur') || DAY), 0);
    const recs = recs0.length > 1 && durSum > DAY * 1.01
      ? [recs0.reduce((a, b) => (lead({ ec: num(b, 'ec'), c: num(b, 'c'), es: num(b, 'es') }) > lead({ ec: num(a, 'ec'), c: num(a, 'c'), es: num(a, 'es') }) ? b : a))]
      : recs0;
    const h = byDay.get(d);
    const rec: Record<string, number> = {};
    for (const k of SUM_KEYS) rec[k] = recs.reduce((s, r) => s + num(r, k), 0);
    // Only when the hours hold clearly more (≥ 1 % and ≥ 1 unit) — rounding never rewrites a day.
    const more = !!h && SUM_KEYS.some((k) => h.sums[k] > rec[k] * 1.01 + 1e-3 && h.sums[k] - rec[k] >= (k === 'c' || k === 'r' || k === 'n' ? 0.01 : 1));
    if (!h || !more || lead(h.sums) <= lead(rec)) { out.push(...recs); return; }
    const fixed: Record<string, unknown> = { ...recs[0], ts: d, ts_end: d + DAY, dur: Math.min(DAY, h.dur), q: num(recs[0], 'q') | 1 };
    for (const k of SUM_KEYS) fixed[k] = Math.max(rec[k], h.sums[k]);
    fixed.reconciled = true;
    out.push(fixed as unknown as Reconciled);
  });
  // Closed days with hours but no day record (not today — it is still running).
  byDay.forEach((h, d) => {
    if (seen.has(d) || d >= today) return;
    const rec: Record<string, unknown> = { ts: d, ts_end: d + DAY, dur: Math.min(DAY, h.dur), q: 1, reconciled: true };
    for (const k of SUM_KEYS) rec[k] = h.sums[k];
    out.push(rec as unknown as Reconciled);
  });
  return out;
}
