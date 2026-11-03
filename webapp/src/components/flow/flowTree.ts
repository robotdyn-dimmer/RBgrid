/**
 * EnergyFlowTree renderer — ported 1:1 from the design handoff `energy-flow-reference.js`
 * (`FL.tree`): single-line diagram ввод (трансформатор + счётчик) → главная шина → щиты
 * (автомат, сумма, стрелка) → линии (автомат QFx.y, неподвижная стрелка, значение, тег).
 * Generation is a second infeed on the main bus (sun + inverter above the bus, arrow down
 * into it), never a feeder among the consumers.
 * Used by Предприятие and Станции (spec §4.2/§4.3/§5). Up to ~10 lines on 760 px.
 *
 * No-flicker (spec §2.3 / §10.2): the skeleton depends only on the STRUCTURE (line set, names,
 * on/off, direction, alarm, no-data) — see {@link treeSignature}; live values are patched in
 * place by {@link treePatch} (text nodes marked `data-fl`), the SVG is never recreated for them.
 */

import { escapeXml as esc } from './escapeXml';
import { t } from '../../i18n';
import { pwKw } from '../common/ruFormat';

export interface TreeNode {
  id: string;
  name: string;
  short?: string;
  /** kW; null = no data (drawn grey/dashed). */
  v: number | null;
  gen?: boolean;
  alarm?: boolean;
  /** Overrides the default `QF{group}.{line}` tag (e.g. a phase label). */
  label?: string;
}
export interface TreeGroup {
  id: string;
  name: string;
  kids: TreeNode[];
}
export interface TreeData {
  /** kW. Signed only when {@link gridHasDirection}. */
  grid: number;
  gridHasDirection: boolean;
  busLabel?: string;
  groups: TreeGroup[];
  /**
   * Generation feeding the main bus (operator 2026-10-13: solar must not read as one more
   * consumer feeder). Drawn ABOVE the bus at its right end — a second infeed with a sun
   * symbol and an arrow INTO the bus.
   */
  sources?: TreeNode[];
  lost?: boolean;
}

const FULL_CSS =
  '.fl text{font-family:var(--font-sans)}.fl .nm{fill:var(--ink-muted);font-size:12px}.fl .vl{fill:var(--ink);font-size:14px;font-weight:600}.fl .tag{fill:var(--ink-muted);font-family:var(--font-mono);font-size:11px}' +
  '.fl .sym{fill:var(--surface);stroke:var(--ink-muted);stroke-width:1.5}.fl .sym-l{stroke:var(--ink-muted);stroke-width:1.5}' +
  '.fl .bus{stroke:var(--ink);stroke-width:4;stroke-linecap:round}.fl .arw{fill:none;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}';

/** kW → "640 Вт" / "1,85 кВт" / «нет данных» in the active locale. */
export function pwt(kw: number | null): string {
  return pwKw(kw);
}

function on(x: number | null): boolean { return x != null && Math.abs(x) > 0.005; }
function arrow(x: number, y: number, up: boolean, c: string): string {
  return up
    ? '<path d="M' + (x - 5) + ' ' + (y + 4) + ' L' + x + ' ' + (y - 4) + ' L' + (x + 5) + ' ' + (y + 4) + '" class="arw" stroke="' + c + '"/>'
    : '<path d="M' + (x - 5) + ' ' + (y - 4) + ' L' + x + ' ' + (y + 4) + ' L' + (x + 5) + ' ' + (y - 4) + '" class="arw" stroke="' + c + '"/>';
}
function brk(x: number, y: number, c?: string | null): string {
  return '<rect x="' + (x - 6) + '" y="' + (y - 6) + '" width="12" height="12" class="sym"' + (c ? ' style="stroke:' + c + '"' : '') + '/>' +
    '<line x1="' + (x - 6) + '" y1="' + (y + 6) + '" x2="' + (x + 6) + '" y2="' + (y - 6) + '" class="sym-l"/>';
}

const gridText = (d: TreeData) => (d.gridHasDirection ? (d.grid < 0 ? '↑ ' : '↓ ') : '') + pwt(Math.abs(d.grid));
const groupSum = (g: TreeGroup) => g.kids.reduce((a, k) => a + (k.gen ? -(k.v || 0) : (k.v || 0)), 0);
// A switchboard none of whose feeders reports has no measured sum — «—», not «0 W» (3φ audit
// L3P-07: «MDB 0 W» under an input importing 218 W).
const groupText = (g: TreeGroup) => { if (g.kids.length && g.kids.every((k) => k.v == null)) return pwt(null); const sum = groupSum(g); return (sum < -0.005 ? '↑ ' : '') + pwt(Math.abs(sum)); };
/** Fit a feeder name into its column (≈ 6,5 px a character at 12 px) — names overlapped (L3P-09). */
const fit = (s: string, px: number): string => { const n = Math.max(4, Math.floor(px / 6.5)); return s.length > n ? s.slice(0, n - 1) + '…' : s; };
const srcText = (k: TreeNode) => pwt(k.v);
const kidText = (k: TreeNode) => (k.v == null ? pwt(null) : (k.gen && on(k.v) ? '↑ ' : '') + pwt(k.v));

interface LKid extends TreeNode { x: number; }
interface LGroup { name: string; kids: LKid[]; }

function treeAria(d: TreeData): string {
  const n = d.groups.reduce((a, g) => a + g.kids.length, 0);
  const gen = (d.sources ?? []).reduce((a, k) => a + (k.v ?? 0), 0);
  return t(d.gridHasDirection && d.grid < 0 ? 'flow.ariaExport' : 'flow.ariaInput', { power: pwt(Math.abs(d.grid)) })
    + (d.sources?.length ? ', ' + t('flow.ariaSolar', { power: pwt(gen) }) : '') + ', ' + t('flow.treeLines', { count: n });
}

/** Full SVG for the current tree (viewBox 0 0 760 330). */
export function treeSkeleton(d: TreeData): string {
  const W = 760, H = 330, busY = 56;
  const exp = d.gridHasDirection && d.grid < 0;
  const gc = exp ? 'var(--flow-export)' : 'var(--flow-import)';

  let o = '<svg class="fl" viewBox="0 0 ' + W + ' ' + H + '" width="100%" role="img" aria-label="' + esc(t('flow.treeAria', { summary: treeAria(d) })) + '"><style>' + FULL_CSS + '</style>';
  o += '<circle cx="26" cy="' + busY + '" r="11" class="sym"/><circle cx="40" cy="' + busY + '" r="11" class="sym"/><line x1="51" x2="80" y1="' + busY + '" y2="' + busY + '" stroke="' + gc + '" stroke-width="2.5"/><rect x="80" y="' + (busY - 13) + '" width="30" height="26" rx="3" class="sym"/><text x="95" y="' + (busY + 4) + '" text-anchor="middle" class="tag">kWh</text><line x1="110" x2="140" y1="' + busY + '" y2="' + busY + '" stroke="' + gc + '" stroke-width="2.5"/>';
  o += '<text x="20" y="' + (busY + 34) + '" class="nm">' + esc(t('flow.input')) + '</text><text x="20" y="' + (busY + 52) + '" class="vl" data-fl="val:grid">' + esc(gridText(d)) + '</text><line x1="140" x2="' + (W - 16) + '" y1="' + busY + '" y2="' + busY + '" class="bus"/>';
  const srcs = d.sources ?? [];
  // Bus label: right end without generation, else next to the input (the right end is the infeed).
  o += srcs.length
    ? '<text x="150" y="' + (busY - 10) + '" class="tag">' + esc(d.busLabel || t('flow.busLabelDefault')) + '</text>'
    : '<text x="' + (W - 16) + '" y="' + (busY - 10) + '" text-anchor="end" class="tag">' + esc(d.busLabel || t('flow.busLabelDefault')) + '</text>';
  srcs.forEach((k, i) => {
    const x = W - 40 - i * 150;
    const live = on(k.v);
    const c = k.v == null ? 'var(--status-offline)' : live ? 'var(--solar)' : 'var(--border-strong)';
    // sun (circle + rays) → inverter box «~» → line down into the bus, arrow pointing into it
    o += '<circle cx="' + x + '" cy="14" r="7" fill="none" stroke="' + c + '" stroke-width="1.75"/>';
    for (let a = 0; a < 8; a++) {
      const r = (a * Math.PI) / 4, c1 = Math.cos(r), s1 = Math.sin(r);
      o += '<line x1="' + (x + c1 * 9.5) + '" y1="' + (14 + s1 * 9.5) + '" x2="' + (x + c1 * 12) + '" y2="' + (14 + s1 * 12) + '" stroke="' + c + '" stroke-width="1.5" stroke-linecap="round"/>';
    }
    o += '<line x1="' + x + '" x2="' + x + '" y1="27" y2="' + busY + '" stroke="' + c + '" stroke-width="2.5"' + (k.v == null ? ' stroke-dasharray="4 4"' : '') + '/>' + (live ? arrow(x, 42, false, c) : '');
    o += '<text x="' + (x - 18) + '" y="18" text-anchor="end" class="nm">' + esc(k.short || k.name) + '</text>' +
      '<text x="' + (x - 18) + '" y="36" text-anchor="end" class="vl" data-fl="val:s' + i + '" style="' + (live ? 'fill:var(--solar-ink, var(--solar))' : 'fill:var(--ink-muted);font-weight:400') + '">' + esc(srcText(k)) + '</text>';
  });

  // layout
  let N = 0; d.groups.forEach((g) => { N += g.kids.length; });
  const sp = Math.min(104, (W - 230) / Math.max(1, N - 1 + (d.groups.length - 1) * 0.4));
  let cx = 200;
  const lgroups: LGroup[] = d.groups.map((g) => ({
    name: g.name,
    kids: g.kids.map((k) => { const lk: LKid = { ...k, x: cx }; cx += sp; return lk; }),
  }));
  // (match reference: inter-group gap applied after each group)
  cx = 200;
  lgroups.forEach((g) => { g.kids.forEach((k) => { k.x = cx; cx += sp; }); cx += sp * 0.4; });

  lgroups.forEach((g, gi) => {
    if (g.kids.length === 0) return;
    const xs = g.kids.map((k) => k.x);
    const gx = xs.reduce((a, b) => a + b, 0) / xs.length;
    const sum = groupSum(d.groups[gi]);
    const up = sum < -0.005;
    const gcol = !on(sum) ? 'var(--border-strong)' : up ? 'var(--flow-export)' : 'var(--primary)';
    const rt = gx > W - 170, lx = rt ? gx - 12 : gx + 12, an = rt ? ' text-anchor="end"' : '';
    o += '<line x1="' + gx + '" x2="' + gx + '" y1="' + busY + '" y2="160" stroke="' + gcol + '" stroke-width="2.5"/>' + brk(gx, 86) + (on(sum) ? arrow(gx, 122, up, gcol) : '');
    o += '<text x="' + lx + '"' + an + ' y="112" class="nm">' + esc(g.name) + '</text><text x="' + lx + '"' + an + ' y="130" class="vl" data-fl="val:g' + gi + '"' + (up ? ' style="fill:var(--flow-export)"' : '') + '>' + esc(groupText(d.groups[gi])) + '</text>';
    o += '<line x1="' + (Math.min(...xs) - 14) + '" x2="' + (Math.max(...xs) + 14) + '" y1="160" y2="160" class="bus" style="stroke-width:3"/>';
    g.kids.forEach((k, j) => {
      const nul = k.v == null;
      const c = k.alarm ? 'var(--sev-alarm)' : !on(k.v) ? 'var(--border-strong)' : k.gen ? 'var(--solar)' : 'var(--primary)';
      o += '<line x1="' + k.x + '" x2="' + k.x + '" y1="160" y2="262" stroke="' + (nul ? 'var(--status-offline)' : c) + '" stroke-width="2"' + (nul ? ' stroke-dasharray="4 4"' : '') + '/>' + brk(k.x, 186, k.alarm ? 'var(--sev-alarm)' : null) + (on(k.v) ? arrow(k.x, 228, !!k.gen, c) : '');
      const nm = k.short || k.name;
      const valStyle = k.alarm ? 'fill:var(--sev-alarm)' : nul ? 'fill:var(--ink-muted);font-weight:400' : k.gen ? 'fill:var(--flow-export)' : '';
      const tag = k.label ?? (k.gen ? t('flow.generationTag') : 'QF' + (gi + 1) + '.' + (j + 1));
      o += '<text x="' + k.x + '" y="284" text-anchor="middle" class="nm"><title>' + esc(nm) + '</title>' + esc(fit(nm, sp - 8)) + '</text>' +
        '<text x="' + k.x + '" y="302" text-anchor="middle" class="vl" data-fl="val:' + gi + '.' + j + '" style="' + valStyle + '">' + esc(kidText(k)) + '</text>' +
        '<text x="' + k.x + '" y="318" text-anchor="middle" class="tag">' + esc(tag) + '</text>';
    });
  });
  return o + '</svg>';
}

/** Structural signature — rebuild the skeleton only when this changes (values are patched). */
export function treeSignature(d: TreeData): string {
  const groups = d.groups
    .map((g) => { const sum = groupSum(g);
      return g.id + ':' + g.name + ':' + (on(sum) ? (sum < -0.005 ? 'u' : 'd') : '0') + '['
        + g.kids.map((k) => `${k.id}:${k.short || k.name}:${k.v == null ? 'n' : on(k.v) ? '1' : '0'}:${k.gen ? 'g' : ''}:${k.alarm ? 'a' : ''}:${k.label ?? ''}`).join(',') + ']'; })
    .join(';');
  const srcs = (d.sources ?? []).map((k) => `${k.id}:${k.short || k.name}:${k.v == null ? 'n' : on(k.v) ? '1' : '0'}`).join(',');
  return [d.lost ? 'lost' : '', d.gridHasDirection ? 'dir' : 'nodir', d.gridHasDirection && d.grid < 0 ? 'exp' : 'imp', d.busLabel ?? '', groups, srcs].join('|');
}

/** Patch live values into an existing tree SVG (same structure) — text + aria only. */
export function treePatch(svg: Element, d: TreeData): void {
  const set = (k: string, v: string) => { const el = svg.querySelector('[data-fl="' + k + '"]'); if (el && el.textContent !== v) el.textContent = v; };
  set('val:grid', gridText(d));
  (d.sources ?? []).forEach((k, i) => set('val:s' + i, srcText(k)));
  d.groups.forEach((g, gi) => {
    set('val:g' + gi, groupText(g));
    g.kids.forEach((k, j) => set('val:' + gi + '.' + j, kidText(k)));
  });
  const aria = t('flow.treeAria', { summary: treeAria(d) });
  if (svg.getAttribute('aria-label') !== aria) svg.setAttribute('aria-label', aria);
}
