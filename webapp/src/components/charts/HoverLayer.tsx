/**
 * HoverLayer — the chart tooltip (charts spec 17.10): a crosshair at the nearest moment and ONE
 * card with every series at that time; mouse, touch (tap / drag) and keyboard (← →). Replaces the
 * per-mark SVG <title>, which showed one series, after a delay, and never on a phone.
 */
import React, { useState } from 'react';

export interface HoverRow { label: string; value: string; color?: string; muted?: boolean }

export const HoverLayer: React.FC<{
  /** Moments that can be pointed at (ascending). */
  ts: number[];
  xOf: (ts: number) => number;
  rows: (ts: number) => HoverRow[];
  title: (ts: number) => string;
  left: number; right: number; top: number; bottom: number;
  ink: string; muted: string; surface: string; border: string;
  ariaLabel: string;
}> = ({ ts, xOf, rows, title, left, right, top, bottom, ink, muted, surface, border, ariaLabel }) => {
  const [i, setI] = useState<number | null>(null);
  if (!ts.length) return null;
  const nearest = (x: number) => {
    let best = 0, bd = Infinity;
    ts.forEach((t, k) => { const d = Math.abs(xOf(t) - x); if (d < bd) { bd = d; best = k; } });
    return best;
  };
  const fromEvent = (e: React.PointerEvent<SVGRectElement>) => {
    const svg = e.currentTarget.ownerSVGElement;
    if (!svg) return;
    const r = svg.getBoundingClientRect();
    const scale = svg.viewBox.baseVal.width / r.width || 1;
    setI(nearest((e.clientX - r.left) * scale));
  };
  const cur = i != null ? ts[i] : null;
  const list = cur != null ? rows(cur) : [];
  const x = cur != null ? xOf(cur) : 0;
  // Card: 8 px padding, 16 px per row; flips to the left of the line near the right edge.
  const cw = Math.min(240, Math.max(140, ...list.map((r) => (r.label.length + r.value.length) * 6.4 + 34), title(cur ?? ts[0]).length * 6.6 + 16));
  const ch = 22 + list.length * 16;
  const cx = x + 12 + cw > right ? x - 12 - cw : x + 12;
  const cy = top + 4;
  return (
    <g>
      <rect x={left} y={top} width={Math.max(0, right - left)} height={Math.max(0, bottom - top)} fill="transparent"
        style={{ touchAction: 'pan-y', cursor: 'crosshair', outline: 'none' }}
        tabIndex={0} role="slider" aria-label={ariaLabel} aria-valuetext={cur != null ? `${title(cur)}: ${list.map((r) => `${r.label} ${r.value}`).join(', ')}` : undefined}
        onPointerMove={fromEvent} onPointerDown={fromEvent} onPointerLeave={(e) => { if (e.pointerType === 'mouse') setI(null); }}
        onKeyDown={(e) => {
          if (e.key === 'ArrowRight') { setI((v) => Math.min(ts.length - 1, (v ?? -1) + 1)); e.preventDefault(); }
          if (e.key === 'ArrowLeft') { setI((v) => Math.max(0, (v ?? ts.length) - 1)); e.preventDefault(); }
          if (e.key === 'Escape') setI(null);
        }}
        onBlur={() => setI(null)} />
      {cur != null && list.length > 0 && (
        <g pointerEvents="none">
          <line x1={x} x2={x} y1={top} y2={bottom} stroke={muted} strokeWidth={1} strokeDasharray="2 3" />
          <rect x={cx} y={cy} width={cw} height={ch} rx={6} fill={surface} stroke={border} />
          <text x={cx + 8} y={cy + 15} fontSize={11} fill={muted}>{title(cur)}</text>
          {list.map((r, k) => (
            <g key={k} transform={`translate(${cx + 8}, ${cy + 22 + k * 16})`}>
              {r.color && <rect x={0} y={4} width={10} height={3} rx={1.5} fill={r.color} opacity={r.muted ? 0.45 : 1} />}
              <text x={r.color ? 16 : 0} y={10} fontSize={11} fill={r.muted ? muted : ink}>{r.label}</text>
              <text x={cw - 16} y={10} fontSize={11} fontWeight={700} textAnchor="end" fill={r.muted ? muted : ink} style={{ fontVariantNumeric: 'tabular-nums' }}>{r.value}</text>
            </g>
          ))}
        </g>
      )}
    </g>
  );
};
