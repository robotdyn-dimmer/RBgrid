/**
 * EnergyFlowSankey renderer — ported 1:1 from the design handoff `energy-flow-reference.js`
 * (`FL.sankey`): sources (Солнце/Сеть) → consumers, ribbon width = kW, solar distributed
 * across loads proportionally, surplus → «В сеть» (spec §5).
 *
 * The ribbon geometry is a continuous function of the values, so there is no in-place patch
 * (unlike EnergyFlowHome's animated dots): the React wrapper rebuilds the SVG only when a
 * rounded value/structure SIGNATURE changes, so tiny jitter never redraws and there is no
 * flicker. Colours come from CSS custom properties set by the wrapper.
 */
import { FlowFrame, consumerColor } from './flowFrame';
import { escapeXml as esc } from './escapeXml';
import { t } from '../../i18n';
import { pwKw } from '../common/ruFormat';

const FULL_CSS =
  '.fl text{font-family:var(--font-sans)}.fl .nm{fill:var(--ink-muted);font-size:12px}.fl .vl{fill:var(--ink);font-size:14px;font-weight:600}.fl .tag{fill:var(--ink-muted);font-family:var(--font-mono);font-size:10px}' +
  '.fl .ni{fill:none;stroke-width:1.75;stroke-linecap:round;stroke-linejoin:round}.fl .sym{fill:var(--surface);stroke:var(--ink-muted);stroke-width:1.5}.fl .sym-l{stroke:var(--ink-muted);stroke-width:1.5}' +
  '.fl .bus{stroke:var(--ink);stroke-width:4;stroke-linecap:round}.fl .arw{fill:none;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}.fl .rib{fill-opacity:.26}.fl .rib:hover{fill-opacity:.5}.fl .grid{stroke:var(--chart-grid)}.fl .tick{fill:var(--ink-muted);font-size:11px}';

/** kW → "640 Вт" / "1,85 кВт" / «нет данных» in the active locale. */
/** Fit a label into `px` (≈ 6,5 px a character at 12 px), ending with «…». */
const fitName = (s: string, px: number): string => { const n = Math.max(4, Math.floor(px / 6.5)); return s.length > n ? s.slice(0, n - 1) + '…' : s; };

export function pw(kw: number | null): string {
  return pwKw(kw);
}

interface Sink { id: string; name: string; v: number; nul?: boolean; color: string; al?: boolean; exp?: boolean; y: number; h: number; o: number; }
interface Src { id: string; name: string; v: number; color: string; y: number; h: number; o: number; }

function sankeyAria(f: FlowFrame): string {
  const parts: string[] = [];
  if (f.solar != null) parts.push(t('flow.ariaSolar', { power: pw(f.solar) }));
  parts.push(t('flow.ariaGrid', { power: pw(Math.max(0, f.grid)) }));
  parts.push(t('flow.ariaHouse', { power: pw(f.house) }));
  return parts.join(', ');
}

/** Full SVG for the current frame (viewBox 0 0 760 300). */
export function sankeySkeleton(f: FlowFrame): string {
  const W = 760, H = 300, T = 14, B = 14, gap = 10, x1 = 168, x2 = 576, bw = 8, SLOT = 22;
  const Sv = f.solar || 0, G = Math.max(0, f.grid), E = f.exportWithoutSource ? 0 : Math.max(0, -f.grid) /* nonsense export with no source (T5) — the banner explains */, Hh = f.house;

  const sinks: Sink[] = f.loads.map((l, i) => ({
    id: 'l' + i, name: l.short || l.name, v: l.v || 0, nul: l.v == null,
    // One semantic palette (UX audit GEN-09): consumers are the import/consumption hue in steps,
    // never the series colours that read as «солнце» / «отдача»; the unmetered rest is grey.
    color: consumerColor(l, i), al: l.alarm, y: 0, h: 0, o: 0,
  }));
  if (E > 0.005) sinks.push({ id: 'exp', name: t('flow.toGrid'), v: E, color: 'var(--flow-export)', exp: true, y: 0, h: 0, o: 0 });

  const srcs: Src[] = [
    { id: 'solar', name: t('flow.solar'), v: Sv, color: 'var(--solar)', y: 0, h: 0, o: 0 },
    { id: 'grid', name: t('flow.grid'), v: G, color: 'var(--flow-import)', y: 0, h: 0, o: 0 },
  ].filter((s) => s.id !== 'solar' || f.solar != null);

  const share = Hh > 0 ? Math.min(1, Sv / Hh) : 0;
  const flows: { s: string; t: string; v: number }[] = [];
  sinks.forEach((k) => {
    if (k.exp) flows.push({ s: 'solar', t: k.id, v: E });
    else {
      if (k.v * share > 0) flows.push({ s: 'solar', t: k.id, v: k.v * share });
      if (k.v * (1 - share) > 0) flows.push({ s: 'grid', t: k.id, v: k.v * (1 - share) });
    }
  });

  const tot = Math.max(Sv + G, Hh + E) || 1;
  const avail = H - T - B - gap * (sinks.length - 1);
  let k = avail / tot;
  for (let it = 0; it < 4; it++) {
    const small = sinks.filter((s) => s.v * k < SLOT).length;
    const bigV = sinks.reduce((a, s) => a + (s.v * k < SLOT ? 0 : s.v), 0);
    if (!bigV) break;
    k = Math.min(avail / tot, (avail - SLOT * small) / bigV);
  }

  let y = T + ((H - T - B) - (srcs.reduce((a, s) => a + Math.max(2, s.v * k), 0) + gap * 4)) / 2;
  srcs.forEach((s) => { s.y = y; s.h = Math.max(2, s.v * k); s.o = 0; y += s.h + gap * 4; });
  y = T;
  sinks.forEach((s) => { s.h = Math.max(2, s.v * k); const sl = Math.max(s.h, SLOT); s.y = y + (sl - s.h) / 2; s.o = 0; y += sl + gap; });

  const mid = (x1 + bw + x2) / 2;
  let defs = '', ribs = '';
  flows.forEach((fl, i) => {
    const s = srcs.find((x) => x.id === fl.s);
    const t = sinks.find((x) => x.id === fl.t);
    if (!s || !t) return;
    const h = fl.v * k, ya = s.y + s.o, yb = t.y + t.o; s.o += h; t.o += h;
    defs += '<linearGradient id="sg' + i + '" gradientUnits="userSpaceOnUse" x1="' + (x1 + bw) + '" x2="' + x2 + '" y1="0" y2="0"><stop offset="0" stop-color="' + s.color + '"/><stop offset="1" stop-color="' + t.color + '"/></linearGradient>';
    ribs += '<path class="rib" fill="url(#sg' + i + ')" d="M' + (x1 + bw) + ' ' + ya + ' C' + mid + ' ' + ya + ',' + mid + ' ' + yb + ',' + x2 + ' ' + yb + ' L' + x2 + ' ' + (yb + h) + ' C' + mid + ' ' + (yb + h) + ',' + mid + ' ' + (ya + h) + ',' + (x1 + bw) + ' ' + (ya + h) + 'Z"><title>' + esc(s.name) + ' → ' + esc(t.name) + ': ' + pw(fl.v) + '</title></path>';
  });

  let o = '<svg class="fl" viewBox="0 0 ' + W + ' ' + H + '" width="100%" role="img" aria-label="' + esc(t('flow.sankeyAria', { summary: sankeyAria(f) })) + '"><style>' + FULL_CSS + '</style><defs>' + defs + '</defs>' + ribs;
  srcs.forEach((s) => {
    o += '<rect x="' + x1 + '" y="' + s.y + '" width="' + bw + '" height="' + s.h + '" rx="2" fill="' + s.color + '"/>' +
      '<text class="nm" x="' + (x1 - 12) + '" y="' + (s.y + s.h / 2 - 3) + '" text-anchor="end">' + esc(s.name) + '</text>' +
      '<text class="vl" x="' + (x1 - 12) + '" y="' + (s.y + s.h / 2 + 14) + '" text-anchor="end">' + (s.v > 0.005 ? pw(s.v) : '0 ' + esc(t('units.W'))) + '</text>';
  });
  sinks.forEach((s) => {
    const cy = s.y + s.h / 2 + 4;
    o += '<rect x="' + x2 + '" y="' + s.y + '" width="' + bw + '" height="' + s.h + '" rx="2" fill="' + (s.nul ? 'var(--status-offline)' : s.color) + '"/>' +
      // The name stops before the right-aligned value (≈ 80 px) — «5 A senso no data» overlapped (H3P-10).
      '<text class="nm" x="' + (x2 + 18) + '" y="' + cy + '"><title>' + esc(s.name) + '</title>' + esc(fitName(s.name, W - 8 - 80 - (x2 + 18))) + '</text>' +
      '<text class="vl" x="' + (W - 8) + '" y="' + cy + '" text-anchor="end" style="' + (s.exp ? 'fill:var(--flow-export)' : s.al ? 'fill:var(--sev-alarm)' : s.nul ? 'fill:var(--ink-muted);font-weight:400' : '') + '">' + (s.nul ? pw(null) : pw(s.v)) + '</text>';
  });
  return o + '</svg>';
}

/** Rounded value/structure signature — the wrapper rebuilds the SVG only when this changes. */
export function sankeySignature(f: FlowFrame): string {
  const r = (v: number | null) => (v == null ? 'n' : Math.round(v * 20) / 20); // 0.05 kW
  const loads = f.loads.map((l) => `${l.id}:${r(l.v)}:${l.alarm ? 'a' : ''}`).join(',');
  return [f.lost ? 'lost' : '', r(f.grid), f.solar != null ? r(f.solar) : 'no', r(f.house), loads].join('|');
}
