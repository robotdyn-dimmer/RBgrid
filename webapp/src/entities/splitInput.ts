/**
 * The input metered as one point per phase (fw 1.1.5, operator 06.11 via root u3i3-per-phase-points):
 * a STD-U3I3 split into three MAIN_SUPPLY points — the main one (L1, `main_unit`) and the others as
 * regular hex units with role MAIN_SUPPLY. The hub sums them into `system`; the app does the same
 * for the input point, so every view that reads «the input» (flow, input limit, phases, balance)
 * sees the whole input, not L1. Each leg becomes a `ph[]` row of the input, as on a grouped U3I3.
 * The leg points stay in the frame: supply points are never lines, so nothing counts twice.
 *
 * Needs frame v2 (`role` per unit). A v1 frame, or an input with no other supply legs, is returned
 * unchanged. Pure.
 */
import type { TelemetryFrame, TelemetryPhase, TelemetryUnit } from '../types/telemetry-units';

const SUM_KEYS = ['p_act', 'p_rea', 'i', 'e_in', 'e_out', 'e_day_in', 'e_day_out', 'cost_day'] as const;
const VBUS_CH = 255;

const num = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v);

/** The phase a leg measures: its jack label «L1»…«L3», else channel + 1. */
const legPhase = (u: TelemetryUnit): number | null => {
  const m = /^L([1-3])$/.exec(u.ch_label ?? '');
  if (m) return Number(m[1]);
  return num(u.ch) && u.ch >= 0 && u.ch <= 2 ? u.ch + 1 : null;
};

/** Hex keys of the input's other phase points (empty when the input isn't split). */
export function splitInputLegs(frame: TelemetryFrame): string[] {
  const main = frame.main_unit;
  if (!main || !frame.units?.[main]) return [];
  return Object.entries(frame.units)
    .filter(([k, u]) => k !== main && k !== frame.vbus_unit && k !== frame.alt_unit
      && (u as TelemetryUnit & { role?: string }).role === 'MAIN_SUPPLY' && u.ch !== VBUS_CH)
    .map(([k]) => k);
}

export function foldSplitInput(frame: TelemetryFrame): TelemetryFrame {
  const legs = splitInputLegs(frame);
  if (!legs.length) return frame;
  const mainKey = frame.main_unit as string;
  const all = [frame.units[mainKey], ...legs.map((k) => frame.units[k])];
  const main: TelemetryUnit = { ...frame.units[mainKey] };
  for (const k of SUM_KEYS) {
    if (all.some((u) => num(u[k]))) (main as unknown as Record<string, number>)[k] = all.reduce((s, u) => s + (num(u[k]) ? u[k] as number : 0), 0);
  }
  if (main.result) {
    const r = { ...main.result };
    for (const k of ['p_act', 'p_rea', 'i'] as const) {
      if (all.some((u) => num(u.result?.[k]))) r[k] = all.reduce((s, u) => s + (num(u.result?.[k]) ? u.result![k] as number : 0), 0);
    }
    main.result = r;
  }
  const ph: TelemetryPhase[] = [];
  for (const u of all) {
    const p = legPhase(u);
    if (p == null || ph.some((x) => x.ph === p)) continue;
    const row: TelemetryPhase = { ph: p };
    if (num(u.v)) row.v = u.v;
    if (num(u.i)) row.i = u.i;
    if (num(u.p_act)) row.p = u.p_act;
    if (num(u.p_rea)) row.q = u.p_rea;
    if (num(u.pf)) row.pf = u.pf;
    ph.push(row);
  }
  if (ph.length >= 2) (main as TelemetryUnit & { ph?: TelemetryPhase[] }).ph = ph.sort((a, b) => a.ph - b.ph);
  // A power factor of the sum isn't the leg's; no voltage of «the input» either beyond L1's.
  delete main.pf;
  main.present = all.every((u) => u.present !== false);
  return { ...frame, units: { ...frame.units, [mainKey]: main } };
}
