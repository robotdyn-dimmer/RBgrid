/**
 * EnergyFlowBalance renderer — ported 1:1 from the design handoff `energy-flow-reference.js`
 * (`FL.balance`): two stacked bars on one kW scale, «Откуда» (Солнце/Сеть) and «Куда»
 * (loads + «В сеть»); a segment label is drawn inside when the segment is ≥ 74 px (spec §5).
 *
 * Geometry is a continuous function of the values → no in-place patch; the wrapper rebuilds
 * the SVG only when a rounded value signature changes.
 *
 * Drawn in REAL pixels for the container width (`viewBox = 0 0 W 176`, `width = W`), so the
 * 12 px labels / 11 px ticks never scale down on narrow screens (review 03 §5.2); segment
 * labels are placed inside only when the segment is ≥ 74 real px, ticks thin out to ≥ 44 px.
 */
import { FlowFrame, consumerColor } from './flowFrame';
import { pw } from './flowSankey';
import { escapeXml as esc } from './escapeXml';
import { t } from '../../i18n';
import { nf } from '../common/ruFormat';

const FULL_CSS =
  '.fl text{font-family:var(--font-sans)}.fl .nm{fill:var(--ink-muted);font-size:12px}.fl .vl{fill:var(--ink);font-size:14px;font-weight:600}.fl .tag{fill:var(--ink-muted);font-family:var(--font-mono);font-size:10px}' +
  '.fl .grid{stroke:var(--chart-grid)}.fl .tick{fill:var(--ink-muted);font-size:11px}';

interface Seg { n: string; v: number; c: string; }

function balanceAria(f: FlowFrame): string {
  const parts: string[] = [];
  if (f.solar != null) parts.push(t('flow.ariaSolar', { power: pw(f.solar) }));
  parts.push(t('flow.ariaGrid', { power: pw(Math.max(0, f.grid)) }));
  parts.push(t('flow.ariaHouse', { power: pw(f.house) }));
  return parts.join(', ');
}

/** Narrowest width the balance is laid out for (smaller containers scroll). */
export const BALANCE_MIN_W = 300;

/** Full SVG for the current frame at `width` real px (viewBox 0 0 W 176; default 760). */
export function balanceSkeleton(f: FlowFrame, width = 760): string {
  const W = Math.max(BALANCE_MIN_W, Math.round(width));
  const X = W < 480 ? 64 : 96, BW = W - X - 12;
  const Sv = f.solar || 0, G = Math.max(0, f.grid), E = f.exportWithoutSource ? 0 : Math.max(0, -f.grid) /* nonsense export with no source (T5) — the banner explains */;
  const tot = Math.max(Sv + G, f.house + E) || 1;
  const sc = BW / tot;

  const row = (y: number, label: string, segs: Seg[]): string => {
    let x = X;
    let s = '<text class="nm" x="0" y="' + (y + 23) + '">' + esc(label) + '</text>';
    segs.forEach((g) => {
      const w = g.v * sc;
      if (w < 0.5) return;
      s += '<rect x="' + x + '" y="' + y + '" width="' + Math.max(w - 2, 1) + '" height="36" rx="4" fill="' + g.c + '"><title>' + esc(g.n) + ': ' + pw(g.v) + '</title></rect>';
      if (w > 74) {
        s += '<text x="' + (x + 8) + '" y="' + (y + 15) + '" style="fill:var(--bg);font-size:12px;font-weight:500">' + esc(g.n) + '</text>' +
          '<text x="' + (x + 8) + '" y="' + (y + 30) + '" style="fill:var(--bg);font-size:12px;font-weight:600">' + pw(g.v) + '</text>';
      }
      x += w;
    });
    return s;
  };

  // Tick step: 0.5/1/2/5/10… kW, the smallest that keeps ticks ≥ 44 px apart.
  const NICE = [0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000];
  const step = NICE.find((st) => st * sc >= 44 && (tot / st) <= 16) ?? Math.ceil(tot / 4);
  let ax = '';
  for (let t = 0; t <= tot + 1e-9; t += step) {
    const x = X + t * sc;
    ax += '<line class="grid" x1="' + x + '" x2="' + x + '" y1="20" y2="150"/>' +
      '<text class="tick" x="' + x + '" y="168" text-anchor="middle">' + nf(t, t % 1 ? 1 : 0) + '</text>';
  }

  const loads: Seg[] = f.loads
    .map((l, i) => ({ n: l.short || l.name, v: l.v || 0, c: consumerColor(l, i) }))
    .sort((a, b) => b.v - a.v);
  if (E > 0.005) loads.push({ n: t('flow.toGrid'), v: E, c: 'var(--flow-export)' });

  return '<svg class="fl" viewBox="0 0 ' + W + ' 176" width="' + W + '" height="176" role="img" aria-label="' + esc(t('flow.balanceAria', { summary: balanceAria(f) })) + '"><style>' + FULL_CSS + '</style>' +
    ax +
    row(26, t('flow.from'), [{ n: t('flow.solar'), v: Sv, c: 'var(--solar)' }, { n: t('flow.grid'), v: G, c: 'var(--flow-import)' }]) +
    row(94, t('flow.to'), loads) +
    '<text class="tick" x="' + (W - 12) + '" y="168" text-anchor="end">' + esc(t('units.kW')) + '</text></svg>';
}

/** Rounded value/structure signature — the wrapper rebuilds the SVG only when this changes. */
export function balanceSignature(f: FlowFrame): string {
  const r = (v: number | null) => (v == null ? 'n' : Math.round(v * 20) / 20);
  const loads = f.loads.map((l) => `${l.id}:${r(l.v)}`).join(',');
  return [f.lost ? 'lost' : '', r(f.grid), f.solar != null ? r(f.solar) : 'no', r(f.house), loads].join('|');
}
