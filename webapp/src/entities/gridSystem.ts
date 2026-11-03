/**
 * The site's mains system — single phase, US split-phase (120/240 V, two legs) or three-phase.
 * Spec П2: a single-phase site is the degenerate case of a multi-phase one, so every phase view
 * reads this one model.
 *
 * Wire (root canon bbf06397, DEVICE_API_SURFACE delta №4): system-config
 * `grid { system: ""|"1P"|"SPLIT"|"3P", nominal_v?, nominal_hz? }` — only `system` is writable
 * (live); nominal_v (L-N) / nominal_hz are a read-only mirror of hardware-config
 * `network.nominal_voltage / nominal_frequency` (written there, applied on reboot; 0 = unset is
 * omitted). `""` / absent → derived here: SPLIT when a point is two-pole L1L2 (5), 3P when the
 * input is 3P or a point sits on L3 / a 3P code, else 1P; V from hardware.network, then the
 * input's `rated_voltage_v`; 230 V / 50 Hz as the last resort.
 *
 * Phase codes (units[].phase, 0..7, labels in schema.enums.phase): 0 unset · 1..3 L1..L3 ·
 * 4 3P (all three) · 5 L1L2 · 6 L2L3 · 7 L3L1 (two-pole loads; 5 = US split 240 V range,
 * dryer, EVSE). Allowed by system (client rule of the contract): 1P {0,1} · SPLIT {0,1,2,5} ·
 * 3P {0..7}.
 */

import { t } from '../i18n';

export type GridKind = '1P' | 'SPLIT' | '3P';

export const PHASE_UNSET = 0;
export const PHASE_3P = 4;
export const PHASE_L1L2 = 5;
export const PHASE_L2L3 = 6;
export const PHASE_L3L1 = 7;
export const PHASE_MAX = 7;

/** Legs of the two-pole codes. */
const TWO_POLE: Record<number, number[]> = { 5: [1, 2], 6: [2, 3], 7: [3, 1] };
export const isTwoPole = (phase?: number): boolean => phase != null && phase in TWO_POLE;

export interface GridSystem {
  kind: GridKind;
  /** Line-to-neutral nominal (V). */
  nominalV: number;
  /** Line-to-line nominal (V): 2·V for split, √3·V for 3P, = V for 1P. */
  lineV: number;
  nominalHz: number;
  /** The phases/legs the site has (1-based): [1] · [1, 2] · [1, 2, 3]. */
  legs: number[];
  /** Read from the device (`system.grid`) rather than derived. */
  fromDevice: boolean;
}

const KINDS: GridKind[] = ['1P', 'SPLIT', '3P'];
const DEFAULT_V = 230;
const DEFAULT_HZ = 50;

export interface GridHints {
  /** Phase codes of the configured units (input first is not required). */
  phases?: (number | undefined)[];
  /** The main input's phase code. */
  inputPhase?: number;
  /** The main input's `rated_voltage_v`. */
  inputRatedV?: number;
  /** hardware-config `network.nominal_frequency` (older shape: top-level `nominal_frequency`). */
  hardwareHz?: number;
  /** hardware-config `network.nominal_voltage` (L-N). */
  hardwareV?: number;
}

const pos = (v: unknown): number | undefined =>
  typeof v === 'number' && Number.isFinite(v) && v > 0 ? v : undefined;

export function legsOf(kind: GridKind): number[] {
  return kind === '3P' ? [1, 2, 3] : kind === 'SPLIT' ? [1, 2] : [1];
}

/** Line-to-line nominals as they are written (IEC 60038 and national): √3 × 230 = 398 reads as an
 *  error to an installer — «230/400 V» (3φ audit I3P-15). */
const LL_NOMINALS = [208, 220, 380, 400, 415, 440, 480, 600, 690];
export function lineVoltage(kind: GridKind, v: number): number {
  if (kind === 'SPLIT') return v * 2;
  if (kind !== '3P') return v;
  const ll = v * Math.sqrt(3);
  const std = LL_NOMINALS.find((n) => Math.abs(n - ll) / n <= 0.02);
  return std ?? Math.round(ll);
}

/** What the points' phases SUGGEST (a hint for the Settings, not the system: the hub counts
 *  an unset `grid.system` as 1P — root canon, F20). */
export function deriveGridKind(h: GridHints): GridKind {
  const all = [...(h.phases ?? []), h.inputPhase];
  if (all.includes(PHASE_L1L2)) return 'SPLIT';
  if ([3, PHASE_3P, PHASE_L2L3, PHASE_L3L1].some((p) => all.includes(p))) return '3P';
  return '1P';
}

/** `system.grid` (any shape, possibly absent) + hints → the site's mains system. */
export function parseGrid(raw: unknown, h: GridHints = {}): GridSystem {
  const o = raw && typeof raw === 'object' && !Array.isArray(raw) ? (raw as Record<string, unknown>) : null;
  const declared = o && typeof o.system === 'string' && (KINDS as string[]).includes(o.system.toUpperCase())
    ? (o.system.toUpperCase() as GridKind) : null;
  // Unset ("" / absent / not loaded yet) = 1P, as the firmware counts it (root F20) — never a
  // multi-phase system the user did not choose. deriveGridKind only suggests in the Settings.
  const kind = declared ?? '1P';
  const exact = pos(o?.nominal_v) ?? pos(h.hardwareV);
  const nominalV = exact ?? pos(h.inputRatedV) ?? DEFAULT_V;
  // A rated_voltage_v guess may be line-to-line: 400/415/480 V on 3P, 240 V on a split service.
  const lnV = !exact && kind === '3P' && nominalV > 300 ? Math.round(nominalV / Math.sqrt(3)) : nominalV;
  const legV = !exact && kind === 'SPLIT' && lnV > 180 ? lnV / 2 : lnV;
  return {
    kind,
    nominalV: legV,
    lineV: lineVoltage(kind, legV),
    nominalHz: pos(o?.nominal_hz) ?? pos(h.hardwareHz) ?? DEFAULT_HZ,
    legs: legsOf(kind),
    fromDevice: declared != null,
  };
}

/** Nominal of the service as installers write it: «230 В» · «120/240 В» · «230/400 В». */
export function mainsVoltageText(g: GridSystem): string {
  const v = t('units.V');
  if (g.kind === 'SPLIT') return `${g.nominalV}/${g.lineV} ${v}`;
  if (g.kind === '3P') return `${g.nominalV}/${g.lineV} ${v}`;
  return `${g.nominalV} ${v}`;
}

/** Phase codes a point may take on this mains system (0 = unset first). */
export function phaseChoices(kind: GridKind): number[] {
  switch (kind) {
    case 'SPLIT': return [PHASE_UNSET, 1, 2, PHASE_L1L2];
    case '3P': return [PHASE_UNSET, 1, 2, 3, PHASE_3P, PHASE_L2L3, PHASE_L3L1, PHASE_L1L2].sort((a, b) => a - b);
    default: return [PHASE_UNSET, 1];
  }
}

/**
 * Conductors a phase code spans: 3P → 3, two-pole (5–7) → 2, else 1. A single-CT point measures
 * ONE of them (root delta №4), so its full power is ≈ P × this, assuming balance (W9).
 */
export const conductorsOf = (phase?: number): number => (phase === PHASE_3P ? 3 : isTwoPole(phase) ? 2 : 1);

/** The legs a phase code loads: 3P → all three, two-pole → its pair, Ln → [n]; legs the site lacks drop. */
export function legsOfPhase(phase: number | undefined, grid: GridSystem): number[] {
  if (!phase) return [];
  const legs = phase === PHASE_3P ? [1, 2, 3] : TWO_POLE[phase] ?? [phase];
  return legs.filter((l) => grid.legs.includes(l));
}

/**
 * Supply-voltage tolerance around the nominal, ± fraction — the firmware's band (thresholds
 * come from hardware `network.nominal_voltage`, root delta №4; EN 50160 ±10 %). Replaces the
 * old fixed 207/253 V (230 V-only) constants.
 */
export const VOLTAGE_TOLERANCE = 0.1;

/** Nominal a reading on `phase` is compared with: a two-pole line sees line-to-line. */
export const nominalFor = (grid: GridSystem, phase?: number): number =>
  (isTwoPole(phase) ? grid.lineV : grid.nominalV);

/** 'low' / 'high' outside nominal ±10 %; null when inside or unknown (no V, or V≈0 = no supply). */
export function voltageBand(v: number | null | undefined, nominal: number): 'low' | 'high' | null {
  if (v == null || !Number.isFinite(v) || v < 1 || !(nominal > 0)) return null;
  if (v < nominal * (1 - VOLTAGE_TOLERANCE)) return 'low';
  if (v > nominal * (1 + VOLTAGE_TOLERANCE)) return 'high';
  return null;
}

/** Mains systems validated on hardware (schema `enums.grid_system_validated`; older hubs → only 1P). */
export const validatedKinds = (fromSchema?: string[]): GridKind[] =>
  (fromSchema?.length ? fromSchema.map((k) => k.toUpperCase()) : ['1P']).filter((k): k is GridKind => (KINDS as string[]).includes(k));

/** A phase code is validated when some validated system offers it (1P: unset and L1). */
export const phaseCodeValidated = (code: number, validated: GridKind[]): boolean =>
  validated.some((k) => phaseChoices(k).includes(code));

/** What the input can actually take: the smaller of its allowed power and what its breaker
 *  passes (A × V × conductors). Undefined when neither is set. */
export function effectiveInputLimitW(ratedW: number | undefined, breakerA: number | undefined, phase: number | undefined, nominalV: number): number | undefined {
  const byBreaker = breakerA && breakerA > 0 ? breakerA * (nominalV || DEFAULT_V) * conductorsOf(phase) : undefined;
  if (ratedW && ratedW > 0 && byBreaker) return Math.min(ratedW, byBreaker);
  return (ratedW && ratedW > 0 ? ratedW : undefined) ?? byBreaker;
}
