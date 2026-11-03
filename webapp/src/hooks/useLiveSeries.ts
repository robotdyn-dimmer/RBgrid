/**
 * useLiveSeries — a small per-unit ring buffer of live values collected since the page opened
 * (sparklines without N history requests to the hub). Appends once per new telemetry frame
 * (the groups array identity changes per frame); honest scope: «since this page was opened».
 */
import { useRef } from 'react';
import { UnitEntityGroup } from '../entities/model';

export function useLiveSeries(
  groups: UnitEntityGroup[],
  value: (g: UnitEntityGroup) => number | null,
  max = 60,
): Map<string, number[]> {
  const series = useRef(new Map<string, number[]>());
  const last = useRef<UnitEntityGroup[] | null>(null);
  if (last.current !== groups) {
    last.current = groups;
    for (const g of groups) {
      const v = value(g);
      if (v == null) continue;
      const s = series.current.get(g.unitKey) ?? [];
      s.push(v);
      if (s.length > max) s.splice(0, s.length - max);
      series.current.set(g.unitKey, s);
    }
  }
  return series.current;
}
