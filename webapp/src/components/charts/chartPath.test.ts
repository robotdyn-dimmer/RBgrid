import { linePath, bandPath, runsOf } from './chartPath';

const nums = (d: string) => (d.match(/-?\d+(\.\d+)?/g) ?? []).map(Number);

describe('chart paths', () => {
  it('monotoneX never leaves the data range (no invented peaks)', () => {
    const p = [{ x: 0, y: 10 }, { x: 10, y: 50 }, { x: 20, y: 52 }, { x: 30, y: 10 }, { x: 40, y: 11 }];
    const d = linePath(p, 'monotoneX');
    expect(d.startsWith('M0.0 10.0 C')).toBe(true);
    const ys = nums(d).filter((_, i) => i % 2 === 1);
    expect(Math.min(...ys)).toBeGreaterThanOrEqual(10);
    expect(Math.max(...ys)).toBeLessThanOrEqual(52);
  });
  it('a step holds each value to the next point, the last to endX, corners rounded', () => {
    const d = linePath([{ x: 0, y: 10 }, { x: 10, y: 20 }], 'step', 20);
    expect(d).toMatch(/^M0\.0 10\.0 L/);
    expect(d).toContain('Q10.0 10.0');
    expect(d.endsWith('L20.0 20.0')).toBe(true);
  });
  it('linear stays a polyline; one point is a move', () => {
    expect(linePath([{ x: 0, y: 1 }, { x: 5, y: 2 }], 'linear')).toBe('M0.0 1.0 L5.0 2.0');
    expect(linePath([{ x: 3, y: 4 }], 'monotoneX')).toBe('M3.0 4.0');
  });
  it('runs split on gaps and missing values', () => {
    const r = runsOf([0, 1, 2, 5, 6, 7], (x) => x, (x) => (x === 6 ? null : x), 1.5);
    expect(r).toEqual([[0, 1, 2], [5], [7]]);
  });
  it('a band closes', () => {
    expect(bandPath([{ x: 0, y: 1 }, { x: 5, y: 1 }], [{ x: 0, y: 3 }, { x: 5, y: 3 }], 'step', 10).endsWith('Z')).toBe(true);
    expect(bandPath([{ x: 0, y: 1 }, { x: 5, y: 1 }, { x: 9, y: 2 }], [{ x: 0, y: 3 }, { x: 5, y: 4 }, { x: 9, y: 3 }], 'monotoneX').endsWith('Z')).toBe(true);
  });
});
