/**
 * EnergyFlowHome renderer — ported 1:1 from the design handoff `energy-flow-reference.js`
 * (`FL.home`), split into a structural SKELETON and an in-place PATCH so live updates never
 * recreate the SVG (spec §2.3 / §14 W2: recreating restarts the SMIL dots → "мельканье").
 *
 *  - {@link homeStructuralKey}: changes only when the STRUCTURE changes (active line set,
 *    directions, power buckets, ring visibility). The React component rebuilds the skeleton
 *    only on this key.
 *  - {@link homeSkeleton}: full SVG for a given structure, with `data-fl` hooks on every value.
 *  - {@link homePatch}: updates texts / stroke-widths / ring dash in place (no DOM rebuild),
 *    so values move smoothly while the dots keep animating on the compositor.
 *
 * Colours come from CSS custom properties (`--ink`, `--flow-export`, …) set by the React
 * wrapper from the active palette, exactly as in the design previews.
 */
import { FlowFrame, FlowLoad, solarShare } from './flowFrame';
import { escapeXml as esc } from './escapeXml';
import { t } from '../../i18n';
import { nf, pwKw } from '../common/ruFormat';

export const ICON: Record<string, string> = {
  tower: 'M12 2v20M5 22l7-20 7 20M7 9h10M6 14h12',
  sun: 'M12 17a5 5 0 1 0 0-10 5 5 0 0 0 0 10zM12 1v2M12 21v2M4.2 4.2l1.4 1.4M18.4 18.4l1.4 1.4M1 12h2M21 12h2M4.2 19.8l1.4-1.4M18.4 5.6l1.4-1.4',
  home: 'M3 10 12 3l9 7v10a1 1 0 0 1-1 1h-5v-6H9v6H4a1 1 0 0 1-1-1z',
  plug: 'M9 2v6M15 2v6M6 8h12v4a6 6 0 0 1-12 0zM12 18v4',
  drop: 'M12 2.7l5.7 5.7a8 8 0 1 1-11.4 0z',
  flame: 'M12 2c1 3 5 5 5 10a5 5 0 0 1-10 0c0-3 2-4 2-7 1 1 2 2 3 4',
  bulb: 'M9 18h6M10 22h4M12 2a7 7 0 0 0-4 12.7V17h8v-2.3A7 7 0 0 0 12 2z',
  car: 'M3 13l2-6h14l2 6v4H3zM6 17v2M18 17v2M7 13h.01M17 13h.01',
  layers: 'M12 3 2 8l10 5 10-5zM2 13l10 5 10-5M2 17.5l10 5 10-5',
};

export const DUR: Record<number, number> = { 1: 6.5, 2: 4.8, 3: 3.6 };

/** kW → "640 Вт" / "1,85 кВт" / «нет данных» in the active locale. */
export function pw(kw: number | null): string {
  return pwKw(kw);
}
/**
 * A load's value: a point that is SUPPLYING (negative P — dir 2 on the wire, e.g. a battery
 * discharging or a reverse-fed line) reads «↑ 300 Вт» in the export colour, never as consumption.
 */
const loadVal = (l: FlowLoad): string => (l.v != null && l.v < 0 ? '↑ ' + pw(Math.abs(l.v)) : pw(l.v)) + (l.alarm ? ' ⚠' : '');
const supplying = (l: FlowLoad): boolean => l.v != null && l.v < 0 && bucket(l.v) > 0;
/** Power-step bucket: 0 none · 1 <1 kW · 2 <2.5 kW · 3 ≥2.5 kW (dot cadence + presence). */
export function bucket(x: number | null): number {
  return x == null ? 0 : Math.abs(x) < 0.005 ? 0 : Math.abs(x) < 1 ? 1 : Math.abs(x) < 2.5 ? 2 : 3;
}
/** Bucket lookup used by the structural keys: (series id, kW) → bucket. */
export type BucketFn = (id: string, v: number | null) => number;
const BOUNDS = [0.005, 1, 2.5];

/**
 * Bucket with hysteresis (spec §10.2: no rebuild when a value jitters around a step): the
 * previous bucket per series is kept until the value moves > 10 % past the boundary.
 * Keep one per component (useRef) — it remembers the last bucket of every series id.
 */
export function hysteresisBuckets(): BucketFn {
  const prev = new Map<string, number>();
  return (id, v) => {
    const b = bucket(v);
    const p = prev.get(id);
    if (p !== undefined && p !== b && v != null) {
      const edge = BOUNDS[Math.min(p, b)];
      if (Math.abs(Math.abs(v) - edge) < edge * 0.1) return p;
    }
    prev.set(id, b);
    return b;
  };
}

export function width(v: number): number {
  return 2 + Math.min(Math.abs(v), 4) * 0.45;
}

const CSS =
  '.fl text{font-family:var(--font-sans)}.fl .nm{fill:var(--ink-muted);font-size:12px}.fl .vl{fill:var(--ink);font-size:14px;font-weight:600}.fl .tag{fill:var(--ink-muted);font-family:var(--font-mono);font-size:11px}' +
  '.fl .ni{fill:none;stroke-width:1.75;stroke-linecap:round;stroke-linejoin:round}.fl .sym{fill:var(--surface);stroke:var(--ink-muted);stroke-width:1.5}' +
  '.fl .bus{stroke:var(--ink);stroke-width:4;stroke-linecap:round}';

/** Structural signature — rebuild the skeleton only when this changes. */
export function homeStructuralKey(f: FlowFrame, bk: BucketFn = (_id, v) => bucket(v)): string {
  const exp = f.grid < 0 && f.gridHasDirection;
  const ring = f.gridHasDirection && f.solar != null;
  const loads = f.loads
    // Names are part of the structure: they are baked into the skeleton, so a rename (or the
    // units-config join landing after the first frame) must rebuild it (review 02 B-24).
    .map((l) => `${l.id}:${l.name}:${bk(l.id, l.v)}:${l.v == null ? 'n' : ''}:${l.alarm ? 'a' : ''}:${l.synthetic ? 's' : ''}:${l.group ? l.group.count : ''}:${supplying(l) ? 'x' : ''}`)
    .join(',');
  if (f.scope) return ['scope', f.scope.id, f.scope.name, f.lost ? 'lost' : '', bk('scope', f.scope.house), loads].join('|');
  return [f.lost ? 'lost' : '', exp ? 'exp' : 'imp', bk('grid', f.grid), f.solar != null ? 'sun' : 'no', bk('solar', f.solar ?? null), ring ? 'ring' : '', loads].join('|');
}

/** Render options that are NOT part of the frame (environment, not data). */
export interface HomeSkeletonOptions {
  /**
   * Draw the SMIL flow dots (default true). False under `prefers-reduced-motion: reduce`
   * (spec §10.5 / §2.3 D3) — no `<animateMotion>` is generated at all, so nothing runs.
   */
  motion?: boolean;
}

function dots(path: string, b: number, color: string, rev: boolean, lost?: boolean, motion = true): string {
  if (!b || lost || !motion) return '';
  let s = '';
  const kp = rev ? ' keyPoints="1;0" keyTimes="0;1" calcMode="linear"' : '';
  for (let n = 0; n < (b >= 2 ? 2 : 1); n++) {
    const begin = -n * DUR[b] / 2;
    s +=
      '<circle r="5" fill="' + color + '" opacity=".22"><animateMotion dur="' + DUR[b] + 's" begin="' + begin + 's" repeatCount="indefinite" path="' + path + '"' + kp + '/></circle>' +
      '<circle r="2.6" fill="' + color + '"><animateMotion dur="' + DUR[b] + 's" begin="' + begin + 's" repeatCount="indefinite" path="' + path + '"' + kp + '/></circle>';
  }
  return s;
}

function line(path: string, v: number | null, color: string, rev: boolean, flKey: string, lost?: boolean, dash?: boolean, motion = true): string {
  const b = bucket(v);
  const base = '<path d="' + path + '" fill="none" stroke="var(--border)" stroke-width="2"' + (dash ? ' stroke-dasharray="4 5"' : '') + '/>';
  const over = b
    ? '<path data-fl="line:' + esc(flKey) + '" d="' + path + '" fill="none" stroke="' + color + '" stroke-width="' + width(v ?? 0) + '" stroke-linejoin="round" stroke-linecap="round" opacity=".9"/>'
    : '';
  return base + over + dots(path, b, color, rev, lost, motion);
}

function card(x: number, y: number, w: number, h: number, icon: string, color: string, name: string, valFl: string, val: string, subFl: string, sub: string, stroke?: string | null): string {
  return (
    '<rect x="' + x + '" y="' + y + '" width="' + w + '" height="' + h + '" rx="12" fill="var(--surface-raised)" stroke="' + (stroke || 'var(--border)') + '"/>' +
    '<g transform="translate(' + (x + 12) + ' ' + (y + h / 2 - 12) + ')"><path class="ni" d="' + ICON[icon] + '" stroke="' + color + '"/></g>' +
    '<text class="nm" x="' + (x + 46) + '" y="' + (y + h / 2 - (sub ? 8 : 2)) + '">' + esc(name) + '</text>' +
    '<text class="vl" data-fl="' + valFl + '" x="' + (x + 46) + '" y="' + (y + h / 2 + (sub ? 10 : 14)) + '">' + esc(val) + '</text>' +
    (sub ? '<text class="tag" data-fl="' + subFl + '" x="' + (x + 46) + '" y="' + (y + h / 2 + 24) + '">' + esc(sub) + '</text>' : '')
  );
}

/**
 * Load-card text budget (viewBox px, RELATIVE to the card's left edge): the card is 192 wide;
 * the name starts at +38 and the value is right-aligned at +182. Widths are estimates (Inter:
 * ~0.54 em per glyph at 12 px, ~0.58 em for the 13 px semibold value) — deterministic, so the
 * skeleton stays a pure string.
 */
const LOAD_NAME_X = 38;
const LOAD_VAL_X = 182;
const NAME_CH = 6.5;
const VAL_CH = 7.6;
/** Widest value the patch can write for this structure (values change in place). */
function loadValueReserve(l: FlowLoad): number {
  const txt = l.v == null ? pw(null) : nf(99.99, 2) + ' ' + t('units.kW');
  return Math.ceil(txt.length * VAL_CH) + (l.alarm ? 16 : 0);
}
/** Truncate to `maxCh` glyphs with an ellipsis. */
export function ellipsize(name: string, maxCh: number): string {
  const chars = Array.from(name);
  if (chars.length <= maxCh) return name;
  return chars.slice(0, Math.max(1, maxCh - 1)).join('').trimEnd() + '…';
}
/** The label a load card shows (a group card carries its member count). */
function cardName(l: FlowLoad): string {
  return l.group ? t('flow.groupCard', { name: l.name, count: l.group.count }) : l.name;
}
/**
 * Lay out a load name against its value: inline when it fits beside the value, else stacked
 * (name on its own line above the value, full card width), ellipsized when still too long.
 * The full name always goes into a `<title>` when shortened.
 */
export function loadNameLayout(l: FlowLoad): { text: string; stacked: boolean; truncated: boolean } {
  const name = cardName(l);
  const inlineCh = Math.floor((LOAD_VAL_X - LOAD_NAME_X - loadValueReserve(l) - 8) / NAME_CH);
  const n = Array.from(name).length;
  if (n <= inlineCh) return { text: name, stacked: false, truncated: false };
  const fullCh = Math.floor((LOAD_VAL_X - LOAD_NAME_X) / NAME_CH);
  const text = ellipsize(name, fullCh);
  return { text, stacked: true, truncated: text !== name };
}

/*
 * Layout — symmetric «butterfly» (operator 2026-10-13: the left side was empty with many loads):
 *   sources (Сеть · Солнце, or the house above a group) in a row ON TOP of the hub;
 *   the hub (house / group) in the middle; the loads split into two columns LEFT and RIGHT of it,
 *   each column centred on the hub. The canvas grows downwards with the number of loads.
 */
const VW = 760;
const CX = VW / 2;
const SRC_Y = 10;
const SRC_H = 68;
/** Source card: the text column starts at +46 and must end inside the card (EN «importing from
 *  grid» ran past the Grid card, 20.10) — flowHome.test checks every locale against SRC_TEXT_W. */
export const SRC_W = 168;
export const SRC_TEXT_W = SRC_W - 46 - 8;
const HUB_R = 40;
const RING_R = 47;
const CARD_W = 192;
const CARD_H = 44;
const PITCH = 56;
const COL_L = 24;
const COL_R = VW - 24 - CARD_W;
const SPINE_L = COL_L + CARD_W + 20;
const SPINE_R = COL_R - 20;

export interface HomeLayout {
  height: number;
  cy: number;
  /** Per load (same order as frame.loads): card x / centre y / side. */
  slots: { x: number; y: number; side: 'L' | 'R' }[];
}

/** Pure geometry for N loads (exported for tests). Left column takes the first ⌈N/2⌉ loads. */
export function homeLayout(n: number): HomeLayout {
  const nl = n <= 1 ? 0 : Math.ceil(n / 2);
  const nr = n - nl;
  const block = (k: number) => Math.max(0, k * PITCH - (PITCH - CARD_H));
  const tallest = Math.max(block(nl), block(nr));
  // The hub sits low enough for the source lines, and the columns (centred on the hub) start
  // below the source row — their spines never run behind the Сеть / Солнце cards.
  const srcBottom = SRC_Y + SRC_H;
  const cy = Math.max(srcBottom + 44 + RING_R, srcBottom + 14 + tallest / 2);
  const col = (k: number, x: number, side: 'L' | 'R') => {
    const top = cy - block(k) / 2;
    return Array.from({ length: k }, (_, i) => ({ x, y: top + CARD_H / 2 + i * PITCH, side }));
  };
  const slots = [...col(nl, COL_L, 'L'), ...col(nr, COL_R, 'R')];
  const height = Math.ceil(Math.max(cy + RING_R + 40, cy + tallest / 2 + 12));
  return { height, cy, slots };
}

/** Hub → load card route, rounded corners at the column spine. */
function loadPath(cy: number, slot: { x: number; y: number; side: 'L' | 'R' }): string {
  const left = slot.side === 'L';
  const x0 = left ? CX - RING_R : CX + RING_R;
  const spine = left ? SPINE_L : SPINE_R;
  const x1 = left ? slot.x + CARD_W : slot.x;
  const d = left ? -1 : 1;
  const y = slot.y;
  if (Math.abs(y - cy) < 1) return 'M' + x0 + ' ' + cy + ' H' + x1;
  const up = y < cy;
  return 'M' + x0 + ' ' + cy + ' H' + (spine - d * 6) + ' Q' + spine + ' ' + cy + ' ' + spine + ' ' + (up ? cy - 6 : cy + 6) +
    ' V' + (up ? y + 6 : y - 6) + ' Q' + spine + ' ' + y + ' ' + (spine + d * 6) + ' ' + y + ' H' + x1;
}

/** Source card (centre x) → hub route: down from the card, then across into the ring. */
function sourcePath(sx: number, cy: number): string {
  const top = SRC_Y + SRC_H;
  if (Math.abs(sx - CX) < 1) return 'M' + CX + ' ' + top + ' V' + (cy - RING_R);
  const ey = cy - 30;
  const ex = sx < CX ? CX - 36 : CX + 36;
  const d = sx < CX ? 1 : -1;
  return 'M' + sx + ' ' + top + ' V' + (ey - 10) + ' Q' + sx + ' ' + ey + ' ' + (sx + d * 10) + ' ' + ey + ' H' + ex;
}

/** Full SVG for the current structure (values embedded; patched afterwards). */
export function homeSkeleton(f: FlowFrame, opts: HomeSkeletonOptions = {}): string {
  const motion = opts.motion !== false;
  const scope = f.scope;
  const hasSolar = !scope && f.solar != null;
  const exp = !scope && f.grid < 0 && f.gridHasDirection;
  const G = Math.abs(f.grid);
  const gc = exp ? 'var(--flow-export)' : 'var(--flow-import)';
  const share = solarShare(f);
  const ring = !scope && f.gridHasDirection && f.solar != null;
  const C = 2 * Math.PI * RING_R;
  const L = homeLayout(f.loads.length);
  const cy = L.cy;

  let s = '<svg class="fl" viewBox="0 0 ' + VW + ' ' + L.height + '" width="100%" role="img" aria-label="' + esc(t('flow.homeAria', { summary: homeAria(f) })) + '"><style>' + CSS + '</style>';

  // ---- source lines (behind the cards) ----
  const gx = hasSolar ? CX - 12 - SRC_W / 2 : CX;
  const sx = CX + 12 + SRC_W / 2;
  if (scope) {
    s += line(sourcePath(CX, cy), scope.house > 0 ? f.house : 0, 'var(--primary)', false, 'up', f.lost, false, motion);
  } else {
    s += line(sourcePath(gx, cy), f.grid, gc, exp, 'grid', f.lost, false, motion);
    // No generation → no Солнце card and no solar line at all (spec §10.3: no solar mentions
    // without generation; «нет генерации» is only for a configured-but-idle solar point).
    if (hasSolar) s += line(sourcePath(sx, cy), f.solar, 'var(--solar)', false, 'solar', f.lost, false, motion);
  }
  f.loads.forEach((l, i) => {
    const x = supplying(l);
    s += line(loadPath(cy, L.slots[i]), l.v, x ? 'var(--flow-export)' : 'var(--primary)', x, 'load:' + l.id, f.lost, l.v == null, motion);
  });

  // ---- source cards ----
  if (scope) {
    s += card(CX - SRC_W / 2, SRC_Y, SRC_W, SRC_H, 'home', 'var(--primary)', t('flow.home'), 'val:up', pw(scope.house), '', '', null);
  } else {
    s += card(gx - SRC_W / 2, SRC_Y, SRC_W, SRC_H, 'tower', gc, t('flow.grid'), 'val:grid', gridValText(f), 'sub:grid', gridSubText(f), exp && f.gridPresent !== false ? 'var(--flow-export)' : null);
    if (hasSolar) s += card(sx - SRC_W / 2, SRC_Y, SRC_W, SRC_H, 'sun', 'var(--solar)', t('flow.solar'), 'val:solar', (f.solar ?? 0) > 0.005 ? pw(f.solar) : t('flow.noGeneration'), '', '', null);
  }

  // ---- hub: house ring (only with direction + solar) + centre ----
  if (ring) {
    s += '<circle cx="' + CX + '" cy="' + cy + '" r="' + RING_R + '" fill="none" stroke="var(--flow-import)" stroke-width="5"/>';
    s += '<circle data-fl="ring" cx="' + CX + '" cy="' + cy + '" r="' + RING_R + '" fill="none" stroke="var(--solar)" stroke-width="5" stroke-dasharray="' + (share * C) + ' ' + C + '" transform="rotate(-90 ' + CX + ' ' + cy + ')"/>';
  } else if (scope) {
    s += '<circle cx="' + CX + '" cy="' + cy + '" r="' + RING_R + '" fill="none" stroke="var(--primary)" stroke-width="2" stroke-dasharray="3 4"/>';
  }
  s += '<circle cx="' + CX + '" cy="' + cy + '" r="' + HUB_R + '" fill="var(--surface)"/><g transform="translate(' + (CX - 10) + ' ' + (cy - 34) + ') scale(.84)"><path class="ni" d="' + ICON[scope ? 'layers' : 'home'] + '" stroke="var(--primary)"/></g>';
  s += '<text class="vl" data-fl="val:house" x="' + CX + '" y="' + (cy + 12) + '" text-anchor="middle" style="font-size:15px">' + houseText(f) + '</text>';
  const tag = scope ? ellipsize(scope.name, 12) : t('flow.houseTag');
  s += '<text class="tag" x="' + CX + '" y="' + (cy + 28) + '" text-anchor="middle">' + (scope && tag !== scope.name ? '<title>' + esc(scope.name) + '</title>' : '') + esc(tag) + '</text>';
  if (ring) s += '<text class="nm" data-fl="val:share" x="' + CX + '" y="' + (cy + RING_R + 24) + '" text-anchor="middle" style="fill:var(--solar-ink, var(--solar))">' + esc(t('flow.solarShare', { pct: Math.round(share * 100) })) + '</text>';

  // ---- load cards ----
  f.loads.forEach((l, i) => {
    const { x, y } = L.slots[i];
    const stroke = l.alarm ? 'var(--sev-alarm)' : l.group ? 'var(--border-strong)' : 'var(--border)';
    const iconColor = l.v == null ? 'var(--status-offline)' : l.alarm ? 'var(--sev-alarm)' : l.group ? 'var(--primary)' : 'var(--ink-muted)';
    const valStyle = l.alarm ? 'fill:var(--sev-alarm)' : l.v == null ? 'fill:var(--ink-muted);font-weight:400' : supplying(l) ? 'fill:var(--flow-export)' : '';
    // A group card reads as a small stack (a second card peeking out) and opens the group's tab.
    s += l.group ? '<g data-group="' + esc(l.group.id) + '" style="cursor:pointer">' : '<g>';
    if (l.group) s += '<rect x="' + (x + 5) + '" y="' + (y - CARD_H / 2 - 5) + '" width="' + CARD_W + '" height="' + CARD_H + '" rx="10" fill="var(--surface)" stroke="var(--border)"/>';
    s += '<rect x="' + x + '" y="' + (y - CARD_H / 2) + '" width="' + CARD_W + '" height="' + CARD_H + '" rx="10" fill="var(--surface-raised)" stroke="' + stroke + '"/>';
    s += '<g transform="translate(' + (x + 12) + ' ' + (y - 9) + ') scale(.75)"><path class="ni" d="' + ICON[l.icon] + '" stroke="' + iconColor + '"/></g>';
    const nm = loadNameLayout(l);
    const nameY = nm.stacked ? y - 4 : y + 4;
    const valY = nm.stacked ? y + 13 : y + 5;
    s += '<text class="nm" x="' + (x + LOAD_NAME_X) + '" y="' + nameY + '" style="fill:var(--ink)">' +
      (nm.truncated ? '<title>' + esc(cardName(l)) + '</title>' : '') + esc(nm.text) + '</text>';
    s += '<text class="vl" data-fl="val:load:' + esc(l.id) + '" x="' + (x + LOAD_VAL_X) + '" y="' + valY + '" text-anchor="end" style="font-size:13px;' + valStyle + '">' + esc(loadVal(l)) + '</text>';
    s += '</g>';
  });

  return s + '</svg>';
}

function homeAria(f: FlowFrame): string {
  if (f.scope) return t('flow.ariaGroup', { name: f.scope.name, power: pw(f.house), count: f.loads.length });
  const parts = [t(f.grid < 0 && f.gridHasDirection ? 'flow.ariaExport' : 'flow.ariaImport', { power: pw(f.grid) })];
  if (f.solar != null) parts.push(t('flow.ariaSolar', { power: pw(f.solar) }));
  parts.push(t('flow.ariaHouse', { power: pw(f.house) }));
  return parts.join(', ');
}

/** Grid card value: «—» for an absent input, no arrow for no flow (|P| < 5 W) — UX audit T3. */
export function gridValText(f: FlowFrame): string {
  if (f.gridPresent === false) return '—';
  if (Math.abs(f.grid) < 0.005) return pw(0);
  const exp = f.grid < 0 && f.gridHasDirection;
  return (exp ? '↑ ' : '↓ ') + pw(Math.abs(f.grid));
}
function gridSubText(f: FlowFrame): string {
  if (f.gridPresent === false) return t('flow.gridNoData');
  if (Math.abs(f.grid) < 0.005) return t('flow.gridIdle');
  return t(f.grid < 0 && f.gridHasDirection ? 'flow.exportingToGrid' : 'flow.importingFromGrid');
}
export const houseText = (f: FlowFrame): string => (f.houseKnown === false ? '—' : pw(f.house));

/** Patch value nodes in place (no DOM rebuild). Call on every frame with the SAME structure. */
export function homePatch(root: Element, f: FlowFrame): void {
  const set = (fl: string, text: string) => {
    const el = root.querySelector('[data-fl="' + fl + '"]');
    if (el) el.textContent = text;
  };
  const setW = (fl: string, v: number | null) => {
    const el = root.querySelector('[data-fl="line:' + fl + '"]');
    if (el && v != null) el.setAttribute('stroke-width', String(width(v)));
  };
  set('val:house', houseText(f));
  if (f.scope) {
    set('val:up', pw(f.scope.house));
    setW('up', f.house);
  } else {
    set('val:grid', gridValText(f));
    set('sub:grid', gridSubText(f));
    set('val:solar', (f.solar ?? 0) > 0.005 ? pw(f.solar) : t('flow.noGeneration'));
    setW('grid', f.grid);
    setW('solar', f.solar);
    // ring + share
    if (f.gridHasDirection && f.solar != null) {
      const share = solarShare(f);
      const C = 2 * Math.PI * RING_R;
      const ringEl = root.querySelector('[data-fl="ring"]');
      if (ringEl) ringEl.setAttribute('stroke-dasharray', share * C + ' ' + C);
      set('val:share', t('flow.solarShare', { pct: Math.round(share * 100) }));
    }
  }

  f.loads.forEach((l: FlowLoad) => {
    setW('load:' + l.id, l.v);
    set('val:load:' + l.id, loadVal(l));
  });
}
