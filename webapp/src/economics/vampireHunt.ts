/**
 * «Охота на вампиров» (documents/design/HOME_VAMPIRES.md, step 2): the user switches ONE device
 * off and the app measures the step of the input's live power — the mean of a window before
 * against the median of a window after (a settle gap skips the transient; the medians and a robust
 * noise also absorb a switch that falls inside a window). Pure helpers; the
 * dialog feeds them the live frames.
 */

export interface LiveSample {
  /** Browser time, ms. */
  t: number;
  /** Input active power, W (signed: + import). */
  p: number;
  /** Per-phase active power (U3I3 ph[]), W. */
  ph?: { ph: number; p: number }[];
}

export interface StepResult {
  beforeW: number;
  afterW: number;
  /** after − before, W: a device switched off gives a negative step. */
  deltaW: number;
  /** Noise of the readings (standard deviation over both windows), W. */
  noiseW: number;
  /** The smallest step the readings can tell from noise, W. */
  floorW: number;
  significant: boolean;
  /** The phase with the largest step when the input reports phases and one phase carries ≥ 70 %. */
  phase: number | null;
  phaseDeltas: { ph: number; deltaW: number }[];
}

const median = (xs: number[]): number => {
  const v = [...xs].sort((a, b) => a - b), k = v.length >> 1;
  return v.length % 2 ? v[k] : (v[k - 1] + v[k]) / 2;
};
/** Robust spread: 1.4826 × MAD (= σ for normal noise). A few samples of the switch itself inside a
 *  window don't inflate it (Ф6(a) 17.10 on .139: the load stepped 3–6 s after «Выключил»; the
 *  after-window then held the transition, its σ grew to 1,5 W and a 4,6 W step read «not seen»;
 *  by the same rule −33,9 W read «below the noise 38,6 W»). */
export const robustSd = (xs: number[]): number => { const m = median(xs); return 1.4826 * median(xs.map((x) => Math.abs(x - m))); };

/** Samples of [from, to) ms. */
export const windowOf = (samples: LiveSample[], from: number, to: number): LiveSample[] => samples.filter((s) => s.t >= from && s.t < to);

/** Minimum samples in a window to say anything. */
export const MIN_SAMPLES = 5;

/**
 * The step between two windows. `resolutionW` = the input's resolution (unit p_res_w) or the
 * hub's no-flow floor; a step is significant above max(2 × resolution, 3 × noise).
 */
export function stepBetween(before: LiveSample[], after: LiveSample[], resolutionW = 0.5): StepResult | null {
  if (before.length < MIN_SAMPLES || after.length < MIN_SAMPLES) return null;
  const b = before.map((s) => s.p), a = after.map((s) => s.p);
  // Medians: the level each side settled at, whichever side of the click the switch fell.
  const beforeW = median(b), afterW = median(a);
  const deltaW = afterW - beforeW;
  const noiseW = Math.max(robustSd(b), robustSd(a));
  const floorW = Math.max(2 * resolutionW, 3 * noiseW);
  const phases = Array.from(new Set([...before, ...after].flatMap((s) => (s.ph ?? []).map((r) => r.ph)))).sort((x, y) => x - y);
  const phaseDeltas = phases.map((ph) => {
    const pb = before.flatMap((s) => (s.ph ?? []).filter((r) => r.ph === ph).map((r) => r.p));
    const pa = after.flatMap((s) => (s.ph ?? []).filter((r) => r.ph === ph).map((r) => r.p));
    return { ph, deltaW: pb.length && pa.length ? median(pa) - median(pb) : 0 };
  });
  const totalAbs = phaseDeltas.reduce((s, d) => s + Math.abs(d.deltaW), 0);
  const top = phaseDeltas.reduce<{ ph: number; deltaW: number } | null>((m, d) => (!m || Math.abs(d.deltaW) > Math.abs(m.deltaW) ? d : m), null);
  const phase = phases.length > 1 && top && totalAbs > 0 && Math.abs(top.deltaW) / totalAbs >= 0.7 ? top.ph : null;
  return { beforeW, afterW, deltaW, noiseW, floorW, significant: Math.abs(deltaW) >= floorW, phase, phaseDeltas };
}

export interface FoundVampire {
  name: string;
  /** Standby power the device drew (positive), W. */
  w: number;
  phase: number | null;
  /** When it was found, ms. */
  at: number;
}

const KEY = (device: string) => `rbgrid.vampires:${device}`;

/** The list is a per-viewer note in this browser (not the hub's data). */
export function loadVampires(device: string): FoundVampire[] {
  try {
    const v = JSON.parse(localStorage.getItem(KEY(device)) ?? '[]');
    return Array.isArray(v) ? v.filter((x) => x && typeof x.name === 'string' && typeof x.w === 'number') : [];
  } catch { return []; }
}

export function saveVampires(device: string, list: FoundVampire[]): void {
  try { localStorage.setItem(KEY(device), JSON.stringify(list)); } catch { /* per-viewer convenience */ }
}
