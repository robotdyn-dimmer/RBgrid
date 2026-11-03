import { stepBetween, windowOf, loadVampires, saveVampires, LiveSample } from './vampireHunt';

const run = (from: number, n: number, p: (k: number) => number, ph?: (k: number) => { ph: number; p: number }[]): LiveSample[] =>
  Array.from({ length: n }, (_, k) => ({ t: from + k * 1000, p: p(k), ph: ph?.(k) }));

describe('vampire hunt — the step of one device switched off', () => {
  it('a 4 W step on 37 W with ±0,1 W noise is found', () => {
    const before = run(0, 30, (k) => 37 + (k % 2 ? 0.1 : -0.1));
    const after = run(33_000, 30, (k) => 33 + (k % 2 ? 0.1 : -0.1));
    const r = stepBetween(before, after)!;
    expect(r.deltaW).toBeCloseTo(-4, 6);
    expect(r.significant).toBe(true);
  });
  it('0,5 W (Ф6) is found at the hub resolution 0,1 W, lost in 2 W noise', () => {
    const quiet = stepBetween(run(0, 30, () => 4.7), run(33_000, 30, () => 4.2), 0.1)!;
    expect(quiet.significant).toBe(true);
    const noisy = stepBetween(run(0, 30, (k) => 37 + (k % 2 ? 2 : -2)), run(33_000, 30, (k) => 36.5 + (k % 2 ? 2 : -2)), 0.1)!;
    expect(noisy.significant).toBe(false);
  });
  it('a switch that falls inside the after-window is still found (Ф6(a) 17.10: steps came 3–6 s after the click)', () => {
    // 1-s frames as on .139: 33.8 W before, +4.6 W five seconds into the after-window; noise ±0.08 W.
    const nz = (k: number) => [0, 0.08, -0.05, 0.03, -0.08][k % 5];
    const before = run(0, 30, (k) => 33.8 + nz(k));
    const up = stepBetween(before, run(33_000, 30, (k) => (k < 5 ? 33.8 : 38.4) + nz(k)), 0.2)!;
    expect(up.deltaW).toBeCloseTo(4.6, 1);
    expect(up.significant).toBe(true);
    const off = stepBetween(run(0, 30, (k) => 38.8 + nz(k)), run(33_000, 30, (k) => (k < 3 ? 38.8 : 0) + nz(k)), 0.2)!;
    expect(off.deltaW).toBeCloseTo(-38.8, 1);
    expect(off.significant).toBe(true);
  });
  it('the phase is named when one phase carries the step', () => {
    const ph = (l2: number) => () => [{ ph: 1, p: 15 }, { ph: 2, p: l2 }, { ph: 3, p: 12 }];
    const r = stepBetween(run(0, 10, () => 41, ph(14)), run(13_000, 10, () => 37, ph(10)))!;
    expect(r.phase).toBe(2);
    expect(r.phaseDeltas.find((d) => d.ph === 2)!.deltaW).toBeCloseTo(-4, 6);
  });
  it('too few samples → no verdict; windows are half-open', () => {
    expect(stepBetween(run(0, 3, () => 1), run(5000, 30, () => 1))).toBeNull();
    expect(windowOf(run(0, 10, () => 1), 2000, 5000).map((s) => s.t)).toEqual([2000, 3000, 4000]);
  });
  it('the found list is kept per device in this browser', () => {
    saveVampires('http://h', [{ name: 'Приставка', w: 4, phase: 2, at: 1 }]);
    expect(loadVampires('http://h')).toEqual([{ name: 'Приставка', w: 4, phase: 2, at: 1 }]);
    expect(loadVampires('other')).toEqual([]);
  });
});
