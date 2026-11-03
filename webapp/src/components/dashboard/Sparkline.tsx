/**
 * Sparkline — tiny inline trend (decorative: the exact value is in the neighbouring cell, so it
 * is aria-hidden). Needs ≥ 2 points; flat series draw a mid line.
 */
import React from 'react';
import { linePath } from '../charts/chartPath';

export const Sparkline: React.FC<{ values: number[]; color: string; width?: number; height?: number }> = ({
  values, color, width = 64, height = 18,
}) => {
  if (values.length < 2) return null;
  const min = Math.min(...values);
  const max = Math.max(...values);
  const span = max - min || 1;
  const pts = values.map((v, i) => ({ x: (i / (values.length - 1)) * (width - 2) + 1, y: max === min ? height / 2 : height - 1 - ((v - min) / span) * (height - 2) }));
  return (
    <svg width={width} height={height} viewBox={`0 0 ${width} ${height}`} aria-hidden focusable="false" style={{ display: 'block' }}>
      <path d={linePath(pts, 'monotoneX')} fill="none" stroke={color} strokeWidth={1.5} strokeLinejoin="round" strokeLinecap="round" />
    </svg>
  );
};
