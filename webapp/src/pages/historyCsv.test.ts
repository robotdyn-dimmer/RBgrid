import { buildHistoryCsv } from './historyCsv';
import { makeDeviceClock } from '../core/deviceTime';
import type { HistoryRecord } from '../types/history';

const clock = makeDeviceClock('MSK-3');
const ts = Date.UTC(2026, 9, 15, 9, 0) / 1000; // 12:00 MSK
const rec = (o: Record<string, number>) => ({ ts, ...o } as unknown as HistoryRecord);

describe('history CSV for people (W2-2)', () => {
  it('one file, names, local time, kWh with a decimal comma, «;», BOM, a total row', () => {
    const { text, rows } = buildHistoryCsv(
      [{ name: 'Кухня', records: [rec({ ec: 1234.5, es: 0 })] }, { name: 'Бойлер', records: [rec({ ec: 500, es: 0 })] }],
      'energy', clock, 'RUB', { from: ts - 10, to: ts + 10 },
    );
    expect(rows).toBe(2);
    expect(text.startsWith('﻿')).toBe(true);
    const lines = text.slice(1).trim().split('\r\n');
    expect(lines[0]).toBe('Начало интервала (часы хаба, UTC+03:00);Точка;Взято, кВт·ч;Отдано, кВт·ч;Итого, кВт·ч;Длительность, мин;Состояние');
    expect(lines[1]).toBe('2026-10-15 12:00;Кухня;1,235;0,000;1,235;;');
    expect(lines).toContain('Сумма записей за период;Бойлер;0,500;0,000;0,500');
  });

  it('money columns carry the currency; out-of-window records are skipped', () => {
    const { text, rows } = buildHistoryCsv([{ name: 'Ввод', records: [rec({ c: 1.2, r: 0.1 }), { ts: ts - 100, c: 9 } as unknown as HistoryRecord] }],
      'money', clock, 'EUR', { from: ts - 10, to: ts + 10 });
    expect(rows).toBe(1);
    expect(text).toContain('Расход, EUR');
    expect(text).toContain('2026-10-15 12:00;Ввод;1,20;0,10;1,10;;');
  });
  it('a partial hour is marked with its duration; no «-0.000» (I3P-09)', () => {
    const { text } = buildHistoryCsv([{ name: 'Ввод', records: [rec({ ec: 0.0001, es: 0.0004, dur: 1200 })] }],
      'energy', clock, 'EUR', { from: ts - 10, to: ts + 10 }, 3600);
    expect(text).toContain('2026-10-15 12:00;Ввод;0,000;0,000;0,000;20;неполный');
  });
  it('a record inside another of the same point is not summed twice (3φ rerun R01)', () => {
    const { text, rows } = buildHistoryCsv([{ name: 'Ввод', records: [rec({ ec: 800, dur: 3600 }), { ts: ts + 120, ec: 200, dur: 900 } as unknown as HistoryRecord] }],
      'energy', clock, 'EUR', { from: ts - 10, to: ts + 3600 }, 3600);
    expect(rows).toBe(1);
    expect(text).toContain('Сумма записей за период;Ввод;0,800;0,000;0,800');
  });
});

import { csvText, buildSummaryCsv } from './historyCsv';

describe('CSV names never run as formulas (Kimi #8)', () => {
  it.each([['=HYPERLINK("x")', `'=HYPERLINK("x")`], ['+1', "'+1"], ['-cmd', "'-cmd"], ['@SUM(A1)', "'@SUM(A1)"], [' \t=1', "' \t=1"], ['Kitchen', 'Kitchen'], ['L1 − kitchen', 'L1 − kitchen']])('%s', (a, b) => {
    expect(csvText(a)).toBe(b);
  });
  it('a hub name in both files is neutralised, a negative number keeps its minus', () => {
    const { text } = buildHistoryCsv([{ name: '=cmd', records: [rec({ ec: 0, es: 1500 })] }], 'energy', clock, 'RUB', { from: ts - 10, to: ts + 10 });
    const row = text.slice(1).trim().split('\r\n')[1];
    expect(row).toBe("2026-10-15 12:00;'=cmd;0,000;1,500;-1,500;;");
    expect(buildSummaryCsv([{ name: '@x', inWh: 1000, outWh: 0, cost: 1, revenue: 0, input: false }], 'RUB', 'p')).toContain("'@x;");
  });
});
