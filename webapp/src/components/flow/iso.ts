/**
 * Tiny isometric SVG helper (review 11 §7, prototype `_iso.js`): pure vector, no WebGL, colours
 * from CSS tokens (`--iso-*`, role colours). World axes: x → screen right-down, y → screen
 * left-down, z → up; 2:1 "game" isometric. Returns SVG markup strings (no user text inside).
 */
export type Pt3 = [number, number, number?];

export interface Iso {
  p: (x: number, y: number, z?: number) => [number, number];
  poly: (a: Pt3[], fill: string, extra?: string) => string;
  box: (x0: number, y0: number, z0: number, dx: number, dy: number, dz: number, f?: Faces) => string;
  cyl: (x: number, y: number, z0: number, r: number, h: number, f?: { side?: string; shade?: string; top?: string }) => string;
  path: (a: Pt3[]) => string;
  seg: (a: Pt3, b: Pt3, color: string, w?: number) => string;
}
export interface Faces { top?: string; left?: string; right?: string }

const EDGE = ' stroke="var(--iso-edge)" stroke-width=".8" stroke-linejoin="round"';
const f1 = (n: number) => n.toFixed(1);

export function iso(ox: number, oy: number): Iso {
  const C = 0.866;
  const S = 0.5;
  const p = (x: number, y: number, z = 0): [number, number] => [ox + (x - y) * C, oy + (x + y) * S - z];
  const pts = (a: Pt3[]) => a.map((q) => { const r = p(q[0], q[1], q[2] ?? 0); return f1(r[0]) + ',' + f1(r[1]); }).join(' ');
  const poly = (a: Pt3[], fill: string, extra = EDGE) => '<polygon points="' + pts(a) + '" fill="' + fill + '"' + extra + '/>';
  const box = (x0: number, y0: number, z0: number, dx: number, dy: number, dz: number, f: Faces = {}) => {
    const x1 = x0 + dx, y1 = y0 + dy, z1 = z0 + dz;
    return poly([[x0, y1, z0], [x1, y1, z0], [x1, y1, z1], [x0, y1, z1]], f.left ?? 'var(--iso-left)')
      + poly([[x1, y0, z0], [x1, y1, z0], [x1, y1, z1], [x1, y0, z1]], f.right ?? 'var(--iso-right)')
      + poly([[x0, y0, z1], [x1, y0, z1], [x1, y1, z1], [x0, y1, z1]], f.top ?? 'var(--iso-top)');
  };
  const cyl = (x: number, y: number, z0: number, r: number, h: number, f: { side?: string; shade?: string; top?: string } = {}) => {
    const b = p(x, y, z0), t = p(x, y, z0 + h), rx = r * 1.22, ry = r * 0.707;
    return '<path d="M' + f1(b[0] - rx) + ' ' + f1(b[1]) + ' A' + f1(rx) + ' ' + f1(ry) + ' 0 0 0 ' + f1(b[0] + rx) + ' ' + f1(b[1])
      + ' V' + f1(t[1]) + ' H' + f1(b[0] - rx) + 'Z" fill="' + (f.side ?? 'var(--iso-left)') + '" stroke="var(--iso-edge)" stroke-width=".8"/>'
      + '<path d="M' + f1(b[0]) + ' ' + f1(b[1] + ry) + ' A' + f1(rx) + ' ' + f1(ry) + ' 0 0 0 ' + f1(b[0] + rx) + ' ' + f1(b[1])
      + ' V' + f1(t[1]) + ' H' + f1(b[0]) + 'Z" fill="' + (f.shade ?? 'var(--iso-right)') + '"/>'
      + '<ellipse cx="' + f1(t[0]) + '" cy="' + f1(t[1]) + '" rx="' + f1(rx) + '" ry="' + f1(ry) + '" fill="' + (f.top ?? 'var(--iso-top)') + '" stroke="var(--iso-edge)" stroke-width=".8"/>';
  };
  const path = (a: Pt3[]) => a.map((q, i) => { const r = p(q[0], q[1], q[2] ?? 0); return (i ? 'L' : 'M') + f1(r[0]) + ' ' + f1(r[1]); }).join(' ');
  const seg = (a: Pt3, b: Pt3, color: string, w = 1.6) => {
    const A = p(a[0], a[1], a[2] ?? 0), B = p(b[0], b[1], b[2] ?? 0);
    return '<line x1="' + f1(A[0]) + '" y1="' + f1(A[1]) + '" x2="' + f1(B[0]) + '" y2="' + f1(B[1]) + '" stroke="' + color + '" stroke-width="' + w + '" stroke-linecap="round"/>';
  };
  return { p, poly, box, cyl, path, seg };
}

/** Theme tokens for the scene faces (review 11 §7.4, checked in both themes). */
export const ISO_TOKENS = {
  dark: { top: '#2b323d', left: '#1f252d', right: '#171b21', edge: '#3b4250', ground: '#161a1f', groundLine: '#20262e' },
  light: { top: '#ffffff', left: '#e6eaf0', right: '#d3d9e2', edge: '#a9b3c1', ground: '#eef1f5', groundLine: '#dde2e9' },
} as const;
