/**
 * flowScene — the isometric «Сцена» of the Home overview (review 11 §7: option «Схема | Сцена»,
 * ≤ 15 KB, no runtime deps). Pure: frame → SVG markup + label anchors. The React layer
 * (EnergySceneHome) injects the SVG once per structural key and draws the numbers as a FLAT
 * HTML layer at the anchors — numbers are never skewed or scaled (§7.2).
 *
 * Magnitude is encoded ONLY by line thickness and text (as in the flat schema); object size
 * means nothing. No generation → no panels, no solar cable, no solar label (spec §10.3).
 * The SVG carries no user/device text (names live in the HTML layer) → nothing to escape.
 */
import { FlowFrame, FlowLoad } from './flowFrame';
import { bucket, width, DUR, BucketFn } from './flowHome';
import { iso, Pt3 } from './iso';
import { t } from '../../i18n';

export const SCENE_W = 880;
export const SCENE_H = 500;
/** Loads drawn as objects; the rest stay in the flat schema / legend (collisions, review 11 C′). */
export const SCENE_MAX_LOADS = 5;

export interface SceneAnchor {
  id: string;
  /** Anchor in viewBox units. */
  x: number;
  y: number;
  /** Label sits left of the anchor (right-aligned). */
  left?: boolean;
  /** Extra screen-space offset in px. */
  dx: number;
}

export interface SceneBuild {
  svg: string;
  anchors: SceneAnchor[];
  /** Loads shown as objects (in label order after grid/solar/house). */
  loads: FlowLoad[];
}

/**
 * Which loads get an object: biggest first, the unmetered rest («Без датчика») ALWAYS kept — it
 * can be most of the house (UX audit GEN-16: 13,3 kW = 75 % vanished when there were > 5 points).
 * Real points beyond the cap fold into the rest so the scene still adds up.
 */
export function sceneLoads(f: FlowFrame): FlowLoad[] {
  const real = f.loads.filter((l) => !l.synthetic).sort((a, b) => Math.abs(b.v ?? 0) - Math.abs(a.v ?? 0));
  const other = f.loads.filter((l) => l.synthetic);
  if (real.length + other.length <= SCENE_MAX_LOADS) return [...real, ...other];
  const keep = real.slice(0, SCENE_MAX_LOADS - 1);
  const rest = [...real.slice(SCENE_MAX_LOADS - 1), ...other];
  const v = rest.reduce((s, l) => s + (l.v ?? 0), 0);
  const folded: FlowLoad = other.length && rest.length === other.length
    ? other[0]
    : { id: '__rest__', name: t('flow.rest', { n: rest.length }), icon: 'plug', v, synthetic: true };
  return [...keep, { ...folded, v }];
}

/** Structural signature: rebuild only when this changes (values move inside a bucket in place). */
export function sceneStructuralKey(f: FlowFrame, bk: BucketFn = (_id, v) => bucket(v)): string {
  const exp = f.grid < 0 && f.gridHasDirection;
  return [
    f.lost ? 'lost' : '', exp ? 'exp' : 'imp', bk('grid', f.grid), f.solar != null ? 'sun' : 'no', bk('solar', f.solar),
    sceneLoads(f).map((l) => `${l.id}:${l.icon}:${bk(l.id, l.v)}:${l.alarm ? 'a' : ''}`).join(','),
  ].join('|');
}

function cable(d: string, v: number | null, color: string, rev: boolean, motion: boolean, lost?: boolean): string {
  const b = bucket(v);
  const w = width(v ?? 0);
  let s = '<path d="' + d + '" fill="none" stroke="var(--border-strong)" stroke-width="' + (w + 2).toFixed(1) + '" stroke-linecap="round" stroke-linejoin="round" opacity=".55"/>';
  if (b) s += '<path d="' + d + '" fill="none" stroke="' + color + '" stroke-width="' + w.toFixed(1) + '" stroke-linecap="round" stroke-linejoin="round"/>';
  if (b && motion && !lost) {
    const kp = rev ? ' keyPoints="1;0" keyTimes="0;1" calcMode="linear"' : '';
    for (let n = 0; n < (b >= 2 ? 2 : 1); n++) {
      s += '<circle r="2.8" fill="' + color + '" stroke="var(--bg)" stroke-width="1"><animateMotion dur="' + DUR[b] + 's" begin="'
        + (-n * DUR[b] / 2) + 's" repeatCount="indefinite" path="' + d + '"' + kp + '/></circle>';
    }
  }
  return s;
}

/** Build the scene. `motion=false` under prefers-reduced-motion: no `<animateMotion>` at all. */
export function buildScene(f: FlowFrame, opts: { motion?: boolean; aria?: string } = {}): SceneBuild {
  const motion = opts.motion !== false;
  const I = iso(350, 200);
  const { p } = I;
  const exp = f.grid < 0 && f.gridHasDirection;
  const gc = exp ? 'var(--flow-export)' : 'var(--flow-import)';
  const hasSolar = f.solar != null;
  const loads = sceneLoads(f);

  let s = '<svg viewBox="0 0 ' + SCENE_W + ' ' + SCENE_H + '" width="100%" role="img"'
    + (opts.aria ? ' aria-label="' + opts.aria.replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' } as Record<string, string>)[c]) + '"' : '')
    + (f.lost ? ' opacity=".5"' : '') + '>';
  // ground slab + grid lines
  s += I.poly([[-100, -215, 0], [365, -215, 0], [365, 215, 0], [-100, 215, 0]], 'var(--iso-ground)', ' stroke="var(--iso-ground-line)" stroke-width="1"');
  for (let g = -40; g <= 340; g += 60) s += '<path d="' + I.path([[g, -215], [g, 215]]) + '" stroke="var(--iso-ground-line)" stroke-width=".7" fill="none"/>';

  // pylon (grid)
  const px = -60, py = 190;
  const pc = 'var(--flow-import)';
  s += I.seg([px - 8, py - 8, 0], [px, py, 92], pc) + I.seg([px + 8, py + 8, 0], [px, py, 92], pc)
    + I.seg([px - 8, py + 8, 0], [px, py, 92], pc) + I.seg([px + 8, py - 8, 0], [px, py, 92], pc);
  s += I.seg([px - 20, py + 20, 70], [px + 20, py - 20, 70], pc, 2) + I.seg([px - 14, py + 14, 50], [px + 14, py - 14, 50], pc, 2);

  // house: box + gable roof (panels on the front slope only when there is generation)
  const hx0 = -60, hy0 = -45, hw = 120, hd = 90, hh = 58, rr = 30, yc = 0;
  s += I.box(hx0, hy0, 0, hw, hd, hh);
  s += I.poly([[hx0, hy0, hh], [hx0 + hw, hy0, hh], [hx0 + hw, yc, hh + rr], [hx0, yc, hh + rr]], 'var(--iso-top)');
  s += I.poly([[hx0 + hw, hy0, hh], [hx0 + hw, hy0 + hd, hh], [hx0 + hw, yc, hh + rr]], 'var(--iso-right)');
  s += I.poly([[hx0, hy0 + hd, hh], [hx0 + hw, hy0 + hd, hh], [hx0 + hw, yc, hh + rr], [hx0, yc, hh + rr]], 'var(--iso-left)');
  if (hasSolar) {
    const onSlope = (u: number, v: number): Pt3 => [hx0 + 8 + u * (hw - 16), hy0 + hd - 6 - v * (hd / 2 - 12), hh + 3 + v * (rr - 6)];
    for (let a = 0; a < 4; a++) for (let b = 0; b < 2; b++) {
      const u0 = a / 4 + 0.01, u1 = (a + 1) / 4 - 0.01, v0 = b / 2 + 0.02, v1 = (b + 1) / 2 - 0.02;
      s += I.poly([onSlope(u0, v0), onSlope(u1, v0), onSlope(u1, v1), onSlope(u0, v1)], 'var(--solar)', ' fill-opacity=".85" stroke="var(--iso-right)" stroke-width=".8"');
    }
  }
  s += I.poly([[-20, 45, 0], [2, 45, 0], [2, 45, 30], [-20, 45, 30]], 'var(--iso-right)');
  s += I.poly([[18, 45, 24], [44, 45, 24], [44, 45, 42], [18, 45, 42]], 'var(--primary)', ' fill-opacity=".35" stroke="var(--iso-edge)" stroke-width=".8"');
  s += I.poly([[-48, 45, 24], [-30, 45, 24], [-30, 45, 42], [-48, 45, 42]], 'var(--primary)', ' fill-opacity=".35" stroke="var(--iso-edge)" stroke-width=".8"');

  // cables: grid → switchboard; roof → switchboard; switchboard → loads (diagonal row)
  const sb: [number, number] = [75, 60];
  s += cable(I.path([[px, py], [sb[0], py], [sb[0], sb[1]]]), Math.abs(f.grid), gc, exp, motion, f.lost);
  if (hasSolar) s += cable(I.path([[hx0 + hw, 30, hh + 4], [hx0 + hw, 30, 2], [sb[0], 30, 0], [sb[0], sb[1], 0]]), f.solar, 'var(--solar)', false, motion, f.lost);
  const n = loads.length;
  const step = n > 1 ? Math.min(75, 225 / (n - 1)) : 0;
  const slots: [number, number][] = loads.map((_, i) => [115 + i * step, -185 + i * step]);
  loads.forEach((l, i) => {
    const q = slots[i];
    const color = l.alarm ? 'var(--sev-alarm)' : l.v == null ? 'var(--status-offline)' : 'var(--primary)';
    s += cable(I.path([[sb[0], sb[1]], [sb[0], q[1]], [q[0], q[1]]]), l.v, color, (l.v ?? 0) < 0, motion, f.lost);
  });

  // switchboard cabinet
  s += I.box(sb[0] - 8, sb[1] - 14, 0, 16, 28, 44);
  s += I.poly([[sb[0] + 8, sb[1] - 10, 30], [sb[0] + 8, sb[1] + 10, 30], [sb[0] + 8, sb[1] + 10, 38], [sb[0] + 8, sb[1] - 10, 38]], 'var(--primary)', ' fill-opacity=".6"');

  // load objects: neutral solids, role accent on the top only
  loads.forEach((l, i) => {
    const [x, y] = slots[i];
    const acc = l.alarm ? 'var(--sev-alarm)' : l.v == null ? 'var(--status-offline)' : 'var(--primary)';
    switch (l.icon) {
      case 'drop':
        s += I.cyl(x, y, 0, 13, 44);
        { const c = p(x, y, 44); s += '<ellipse cx="' + c[0].toFixed(1) + '" cy="' + c[1].toFixed(1) + '" rx="15.9" ry="9.2" fill="none" stroke="' + acc + '" stroke-width="1.6"/>'; }
        break;
      case 'flame':
        s += I.box(x - 6, y - 20, 0, 12, 40, 28, { top: acc });
        for (let fin = -16; fin <= 16; fin += 8) s += I.seg([x + 6, y + fin, 4], [x + 6, y + fin, 26], 'var(--border-strong)', 1.2);
        break;
      case 'car':
        s += I.box(x - 5, y - 30, 0, 10, 10, 34, { top: acc });
        s += I.box(x - 14, y - 12, 0, 28, 40, 11) + I.box(x - 12, y - 4, 11, 24, 22, 9);
        break;
      case 'sun':
        s += I.box(x - 18, y - 18, 0, 36, 36, 6, { top: 'var(--solar)' });
        break;
      default: {
        s += I.box(x - 16, y - 16, 0, 32, 32, 26, { top: acc });
      }
    }
  });
  s += '</svg>';

  const anchors: SceneAnchor[] = [];
  const at = (id: string, xy: [number, number], dx: number, left?: boolean) => anchors.push({ id, x: xy[0], y: xy[1], dx, left });
  at('grid', p(px - 20, py + 20, 70), -10, true);
  if (hasSolar) at('solar', p(hx0, -10, hh + rr), -12, true);
  at('house', p(hx0 + hw, yc, hh + rr + 14), 6);
  loads.forEach((l, i) => at(l.id, p(slots[i][0], slots[i][1] - 16, 30), 40));
  return { svg: s, anchors, loads };
}
