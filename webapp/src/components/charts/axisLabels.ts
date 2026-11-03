/**
 * Axis labels by the tick STEP, not by the value's usual precision (charts audit O3: «0,1 / 0,1 /
 * 0,0 / 0,0 kWh» on a fresh hub, «225 / 228 / 230 / 233 V» at a 2.5 V step). The value formatter
 * picks the unit and its scale (Wh → kWh); when its rounding loses more than a tenth of the step,
 * the numbers are redone with as many decimals as the step needs, in the same unit.
 */
export function stepLabels(ticks: number[], fmt: (v: number) => string, locale: string): string[] {
  const plain = ticks.map(fmt);
  if (ticks.length < 2) return plain;
  const dec = (1.5).toLocaleString(locale).replace(/\d/g, '');
  const parse = (s: string): number | null => {
    const m = s.replace(/[\s  ]/g, '').match(/[-−]?\d[\d.,]*/);
    if (!m) return null;
    const raw = m[0].replace('−', '-');
    const norm = dec === ',' ? raw.replace(/\./g, '').replace(',', '.') : raw.replace(/,/g, '');
    const n = Number(norm);
    return Number.isFinite(n) ? n : null;
  };
  const ref = ticks.reduce((a, b) => (Math.abs(b) > Math.abs(a) ? b : a), 0);
  const refStr = fmt(ref);
  const refNum = parse(refStr);
  if (!ref || refNum == null) return plain;
  // The unit's scale: the candidate closest to what the formatter printed for the biggest tick.
  const scale = [1, 1e-3, 1e-6, 1e3].reduce((best, k) => (Math.abs(ref * k - refNum) < Math.abs(ref * best - refNum) ? k : best), 1);
  const step = Math.abs(ticks[1] - ticks[0]) * scale;
  const lossy = ticks.some((v, i) => { const n = parse(plain[i]); return n == null || Math.abs(n - v * scale) > step / 10; });
  if (!lossy) return plain;
  const unit = refStr.replace(/^[^\d−-]*[-−]?\d[\d\s  .,]*/, '').trim();
  let d = 0;
  while (d < 4 && Math.abs(Math.round(step * 10 ** d) - step * 10 ** d) > 1e-6) d++;
  return ticks.map((v) => `${(v * scale).toLocaleString(locale, { minimumFractionDigits: d, maximumFractionDigits: d })}${unit ? ` ${unit}` : ''}`);
}
