/**
 * History export for people (UX audit W2-2: RES-08/09/10). ONE file for every selected point:
 * local date-time of the hub, the point's NAME, kWh / money in the reader's number format, a
 * total per point; «;» as the separator and a UTF-8 BOM when the UI language uses the decimal
 * comma (a Russian Excel opens a «,»-separated file as a single column).
 */
import type { HistoryRecord } from '../types/history';
import type { DeviceClock } from '../core/deviceTime';
import { t, intlLocale } from '../i18n';

export type CsvMetric = 'energy' | 'money' | 'power' | 'voltage' | 'reactive';

export interface CsvSeries { name: string; records: HistoryRecord[] }

const num = (r: HistoryRecord, k: string): number | null => {
  const v = (r as unknown as Record<string, unknown>)[k];
  return typeof v === 'number' && Number.isFinite(v) ? v : null;
};

const pad = (n: number) => String(n).padStart(2, '0');

/** A point name is the hub's text: one starting like a formula (=, +, -, @, after blanks or control
 *  characters) gets a leading apostrophe so a spreadsheet shows it rather than runs it (Kimi #8).
 *  Only names — numbers stay numbers, a negative value keeps its minus. */
export const csvText = (s: string): string => (/^[\s\x00-\x1f]*[=+\-@]/.test(s) ? `'${s}` : s);

export function buildHistoryCsv(
  series: CsvSeries[], metric: CsvMetric, clock: DeviceClock, currency: string,
  span: { from: number; to: number },
  /** The period's nominal length, s (3600 / 86400 / …) — a shorter record is marked partial. */
  nominalS?: number,
): { text: string; rows: number } {
  const comma = (1.5).toLocaleString(intlLocale()).includes(',');
  const sep = comma ? ';' : ',';
  // A value that rounds to zero prints «0.000», never «-0.000» (3φ audit I3P-09).
  const nf = (v: number | null, d: number) => (v == null ? '' : (Math.abs(v) < 0.5 * 10 ** -d ? 0 : v).toLocaleString(intlLocale(), { minimumFractionDigits: d, maximumFractionDigits: d, useGrouping: false }));
  // The hub's UTC offset, so an accountant can place «2026-10-16 01:00» (I3P-09: «hub clock» alone).
  const offsetOf = (t0: number) => { const ts = Math.floor(t0 / 60) * 60; const p = clock.parts(ts); const m = Math.round((Date.UTC(p.y, p.mo - 1, p.d, p.h, p.mi) / 1000 - ts) / 60); return `UTC${m < 0 ? '−' : '+'}${pad(Math.floor(Math.abs(m) / 60))}:${pad(Math.abs(m) % 60)}`; };
  const cell = (s: string) => (s.includes(sep) || /["\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s);
  const when = (ts: number) => { const p = clock.parts(ts); return `${p.y}-${pad(p.mo)}-${pad(p.d)} ${pad(p.h)}:${pad(p.mi)}`; };

  const head = metric === 'energy' ? [t('csv.in'), t('csv.out'), t('csv.net')]
    : metric === 'money' ? [t('csv.cost', { cur: currency }), t('csv.revenue', { cur: currency }), t('csv.netCost', { cur: currency })]
      : metric === 'power' ? [t('csv.power'), t('csv.min'), t('csv.max')]
        : metric === 'reactive' ? [t('csv.reactive'), t('csv.pf'), t('csv.apparent')]
          : [t('csv.voltage'), t('csv.vmin'), t('csv.vmax')];
  const lines: string[] = [[t('csv.timeStart', { tz: offsetOf(span.from) }), t('csv.point'), ...head, t('csv.durationMin'), t('csv.status')].map(cell).join(sep)];
  let rows = 0;
  for (const s of series) {
    const inSpan = s.records.filter((r) => r.ts >= span.from && r.ts < span.to).sort((a, b) => a.ts - b.ts);
    // A record lying INSIDE another of the same point (17:02–17:17 within 17:00–18:00 after a restart
    // test) is the same energy twice — not summed into the file (3φ rerun R01).
    const end = (r: HistoryRecord) => r.ts + (num(r, 'dur') ?? 0);
    const recs = inSpan.filter((r) => !inSpan.some((o) => o !== r && (num(o, 'dur') ?? 0) > (num(r, 'dur') ?? 0) && o.ts <= r.ts && end(o) >= end(r)));
    let a = 0, b = 0;
    for (const r of recs) {
      let vals: string[];
      if (metric === 'energy') {
        const i = (num(r, 'ec') ?? 0) / 1000, o = (num(r, 'es') ?? 0) / 1000;
        a += i; b += o; vals = [nf(i, 3), nf(o, 3), nf(i - o, 3)];
      } else if (metric === 'money') {
        const c = num(r, 'c') ?? 0, rv = num(r, 'r') ?? 0;
        a += c; b += rv; vals = [nf(c, 2), nf(rv, 2), nf(c - rv, 2)];
      } else if (metric === 'power') {
        vals = [nf(num(r, 'p_act'), 1), nf(num(r, 'p_min'), 1), nf(num(r, 'p_max'), 1)];
      } else if (metric === 'reactive') {
        vals = [nf(num(r, 'p_rea'), 1), nf(num(r, 'pf'), 2), nf(num(r, 'p_app'), 1)];
      } else {
        vals = [nf(num(r, 'v'), 1), nf(num(r, 'v_min'), 1), nf(num(r, 'v_max'), 1)];
      }
      const dur = num(r, 'dur');
      const partial = nominalS != null && dur != null && dur < nominalS - 60;
      lines.push([when(r.ts), csvText(s.name), ...vals, dur != null ? nf(dur / 60, 0) : '', partial ? t('csv.partial') : ''].map(cell).join(sep));
      rows += 1;
    }
    if ((metric === 'energy' || metric === 'money') && recs.length) {
      const d = metric === 'energy' ? 3 : 2;
      lines.push([t('csv.total'), csvText(s.name), nf(a, d), nf(b, d), nf(a - b, d)].map(cell).join(sep));
    }
  }
  return { text: (comma ? '﻿' : '') + lines.join('\r\n') + '\r\n', rows };
}

export interface SummaryRow { name: string; inWh: number; outWh: number; cost: number; revenue: number; input?: boolean }

/**
 * «Сводка по точкам» for re-billing (rerun 16.10 RES-R11): one row per point — kWh taken,
 * cost, kWh given, revenue — for the period on screen, plus the total; same number format,
 * separator and BOM rules as the detailed export.
 */
export function buildSummaryCsv(rows: SummaryRow[], currency: string, periodText: string): string {
  const comma = (1.5).toLocaleString(intlLocale()).includes(',');
  const sep = comma ? ';' : ',';
  const nf = (v: number, d: number) => v.toLocaleString(intlLocale(), { minimumFractionDigits: d, maximumFractionDigits: d, useGrouping: false });
  const cell = (s: string) => (s.includes(sep) || /["\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s);
  const lines = [
    [t('csv.period'), periodText].map(cell).join(sep),
    [t('csv.point'), t('csv.in'), t('csv.cost', { cur: currency }), t('csv.out'), t('csv.revenue', { cur: currency })].map(cell).join(sep),
  ];
  // The input already contains its lines: the total is over the lines (or the input alone), and
  // the input row is marked as such — never input + lines counted twice.
  const lines_ = rows.filter((r) => !r.input);
  const sumOf = lines_.length ? lines_ : rows;
  const tot = { inWh: 0, outWh: 0, cost: 0, revenue: 0 };
  for (const r of rows) {
    lines.push([csvText(r.input ? `${r.name} (${t('csv.inputRow')})` : r.name), nf(r.inWh / 1000, 3), nf(r.cost, 2), nf(r.outWh / 1000, 3), nf(r.revenue, 2)].map(cell).join(sep));
  }
  for (const r of sumOf) { tot.inWh += r.inWh; tot.outWh += r.outWh; tot.cost += r.cost; tot.revenue += r.revenue; }
  lines.push([lines_.length && lines_.length < rows.length ? t('csv.totalLines') : t('csv.total'), nf(tot.inWh / 1000, 3), nf(tot.cost, 2), nf(tot.outWh / 1000, 3), nf(tot.revenue, 2)].map(cell).join(sep));
  return (comma ? '﻿' : '') + lines.join('\r\n') + '\r\n';
}
