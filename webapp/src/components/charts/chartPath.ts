/**
 * Line shapes for the app's SVG charts (charts audit 2026-10-17, documents/review/charts-2026-10-17):
 * every line was a polyline — a «saw» on week/month and voltage. One place builds them now:
 * - `monotoneX` — Fritsch–Carlson / d3.curveMonotoneX: rounds the joints but never overshoots the
 *   data (no peak or dip the readings don't have) — trends by day/month, voltage;
 * - `step` — an interval mean held over its interval, corners rounded — power per hour;
 * - `linear` — as before (sparse or exact readings).
 * Gaps split a line into runs: a missing bucket is a hole, not a ramp (RES-16).
 */

export type Curve = 'linear' | 'monotoneX' | 'step';
export interface Pt { x: number; y: number }

const f = (v: number) => v.toFixed(1);

/** Split [ts, value] samples into runs at gaps longer than `gap` (same units as ts) or null values. */
export function runsOf<T>(items: T[], tsOf: (it: T) => number, valOf: (it: T) => number | null, gap: number): T[][] {
  const out: T[][] = [];
  let run: T[] = [];
  let prev: number | null = null;
  for (const it of items) {
    const v = valOf(it), ts = tsOf(it);
    if (v == null || (prev != null && ts - prev > gap)) { if (run.length) out.push(run); run = []; }
    prev = ts;
    if (v != null) run.push(it);
  }
  if (run.length) out.push(run);
  return out;
}

const sign = (v: number) => (v < 0 ? -1 : 1);

/** Tangents at each point by Fritsch–Carlson (d3 slope3 / slope2). */
function tangents(p: Pt[]): number[] {
  const n = p.length;
  const t = new Array<number>(n).fill(0);
  if (n < 2) return t;
  const h = (i: number) => p[i + 1].x - p[i].x;
  const s = (i: number) => (h(i) ? (p[i + 1].y - p[i].y) / h(i) : 0);
  for (let i = 1; i < n - 1; i++) {
    const h0 = h(i - 1), h1 = h(i), s0 = s(i - 1), s1 = s(i);
    const q = (s0 * h1 + s1 * h0) / (h0 + h1 || 1);
    t[i] = (sign(s0) + sign(s1)) * Math.min(Math.abs(s0), Math.abs(s1), 0.5 * Math.abs(q)) || 0;
  }
  // Ends: one-sided, bounded the same way.
  const end = (i: number, j: number, tj: number) => { const hh = p[j].x - p[i].x; return hh ? (3 * (p[j].y - p[i].y) / hh - tj) / 2 : 0; };
  t[0] = n > 2 ? end(0, 1, t[1]) : s(0);
  t[n - 1] = n > 2 ? end(n - 2, n - 1, t[n - 2]) : s(0);
  return t;
}

function monotonePath(p: Pt[]): string {
  if (p.length < 3) return linearPath(p);
  const t = tangents(p);
  let d = `M${f(p[0].x)} ${f(p[0].y)}`;
  for (let i = 0; i < p.length - 1; i++) {
    const dx = (p[i + 1].x - p[i].x) / 3;
    d += ` C${f(p[i].x + dx)} ${f(p[i].y + dx * t[i])} ${f(p[i + 1].x - dx)} ${f(p[i + 1].y - dx * t[i + 1])} ${f(p[i + 1].x)} ${f(p[i + 1].y)}`;
  }
  return d;
}

function linearPath(p: Pt[]): string {
  return p.map((q, i) => `${i ? 'L' : 'M'}${f(q.x)} ${f(q.y)}`).join(' ');
}

/** Step: each value held to the next point's x (the last one to `endX`), corners rounded by `r`. */
function stepPath(p: Pt[], endX: number, r = 3): string {
  const v: Pt[] = [];
  p.forEach((q, i) => {
    const x1 = i < p.length - 1 ? p[i + 1].x : endX;
    v.push({ x: q.x, y: q.y }, { x: x1, y: q.y });
  });
  // Drop zero-height risers (equal neighbours) so a flat run is one segment.
  const pts = v.filter((q, i) => i === 0 || !(q.x === v[i - 1].x && q.y === v[i - 1].y));
  if (pts.length < 2) return linearPath(pts);
  let d = `M${f(pts[0].x)} ${f(pts[0].y)}`;
  for (let i = 1; i < pts.length - 1; i++) {
    const a = pts[i - 1], b = pts[i], c = pts[i + 1];
    const lin = Math.hypot(b.x - a.x, b.y - a.y), lout = Math.hypot(c.x - b.x, c.y - b.y);
    const rr = Math.min(r, lin / 2, lout / 2);
    if (rr < 0.5) { d += ` L${f(b.x)} ${f(b.y)}`; continue; }
    const p1 = { x: b.x - ((b.x - a.x) / lin) * rr, y: b.y - ((b.y - a.y) / lin) * rr };
    const p2 = { x: b.x + ((c.x - b.x) / lout) * rr, y: b.y + ((c.y - b.y) / lout) * rr };
    d += ` L${f(p1.x)} ${f(p1.y)} Q${f(b.x)} ${f(b.y)} ${f(p2.x)} ${f(p2.y)}`;
  }
  const last = pts[pts.length - 1];
  return `${d} L${f(last.x)} ${f(last.y)}`;
}

/** One run's path in the chosen curve. `endX` = where a step's last value ends (default: its own x). */
export function linePath(p: Pt[], curve: Curve, endX?: number): string {
  if (!p.length) return '';
  if (p.length === 1) return `M${f(p[0].x)} ${f(p[0].y)}`;
  if (curve === 'monotoneX') return monotonePath(p);
  if (curve === 'step') return stepPath(p, endX ?? p[p.length - 1].x);
  return linearPath(p);
}

/** A step's corner points: each value held to the next x (the last one to `endX`). */
function stepVertices(p: Pt[], endX: number): Pt[] {
  return p.flatMap((q, i) => [{ x: q.x, y: q.y }, { x: i < p.length - 1 ? p[i + 1].x : endX, y: q.y }]);
}

/** A band (min–max corridor) between two runs over the same x, closed. */
export function bandPath(lo: Pt[], hi: Pt[], curve: Curve, endX?: number): string {
  if (lo.length < 2 || hi.length < 2) return '';
  if (curve === 'step') {
    const e = endX ?? hi[hi.length - 1].x;
    const poly = [...stepVertices(hi, e), ...stepVertices(lo, e).reverse()];
    return `${linearPath(poly)} Z`;
  }
  const back = linePath([...lo].reverse(), curve);
  return `${linePath(hi, curve)} L${back.slice(1)} Z`;
}
