import { FlowFrame } from './flowFrame';
import { homeSkeleton, homeStructuralKey, homeLayout, loadNameLayout, ellipsize, SRC_TEXT_W } from './flowHome';
import ruFlow from '../../i18n/locales/ru/flow.json';
import enFlow from '../../i18n/locales/en/flow.json';
import { balanceSkeleton } from './flowBalance';

const base: FlowFrame = {
  grid: 1.2,
  solar: null,
  house: 1.2,
  gridHasDirection: false,
  loads: [
    { id: 'a', name: 'Бойлер', icon: 'drop', v: 0.8 },
    { id: 'b', name: 'rbAmp I2 ch0 (5A)', icon: 'plug', v: null },
  ],
};

describe('homeSkeleton — solar absent (spec §10.3)', () => {
  it('draws no Солнце card, no solar line and never «нет генерации»', () => {
    const svg = homeSkeleton(base);
    expect(svg).not.toContain('Солнце');
    expect(svg).not.toContain('нет генерации');
    expect(svg).not.toContain('val:solar');
    expect(svg).not.toContain('line:solar');
  });

  it('keeps the Солнце card when generation is configured (even idle)', () => {
    const svg = homeSkeleton({ ...base, solar: 0, gridHasDirection: true });
    expect(svg).toContain('Солнце');
    expect(svg).toContain('нет генерации');
    expect(homeStructuralKey({ ...base, solar: 0 })).not.toBe(homeStructuralKey(base));
  });
});

describe('homeSkeleton — motion', () => {
  it('emits SMIL dots by default and none with motion:false (reduced motion)', () => {
    expect(homeSkeleton(base)).toContain('<animateMotion');
    expect(homeSkeleton(base, { motion: false })).not.toContain('<animateMotion');
  });
});

describe('load names vs values', () => {
  it('short names stay inline', () => {
    expect(loadNameLayout(base.loads[0])).toEqual({ text: 'Бойлер', stacked: false, truncated: false });
  });
  it('a long name next to «нет данных» is stacked above the value, not overlapped', () => {
    const l = loadNameLayout(base.loads[1]);
    expect(l.stacked).toBe(true);
  });
  it('very long names are ellipsized with the full name in <title>', () => {
    const long = 'Очень длинное название линии щита номер семь';
    const svg = homeSkeleton({ ...base, loads: [{ id: 'x', name: long, icon: 'plug', v: 1 }] });
    expect(svg).toContain('<title>' + long + '</title>');
    expect(svg).toContain('…');
    expect(ellipsize('abcdef', 4)).toBe('abc…');
  });
});

describe('balanceSkeleton — pixel width', () => {
  it('draws at the requested real width (no viewBox scaling)', () => {
    const svg = balanceSkeleton({ ...base, solar: 0.5 }, 343);
    expect(svg).toContain('viewBox="0 0 343 176" width="343"');
  });
});

describe('homeLayout — symmetric butterfly', () => {
  it('splits the loads into two columns around the hub (left takes the extra one)', () => {
    const L = homeLayout(5);
    expect(L.slots.filter((s) => s.side === 'L')).toHaveLength(3);
    expect(L.slots.filter((s) => s.side === 'R')).toHaveLength(2);
    // each column is centred on the hub
    const ys = (side: 'L' | 'R') => L.slots.filter((s) => s.side === side).map((s) => s.y);
    for (const side of ['L', 'R'] as const) {
      const v = ys(side);
      expect((v[0] + v[v.length - 1]) / 2).toBeCloseTo(L.cy, 5);
    }
  });
  it('a single load goes to the right; many loads grow the canvas, never into the source row', () => {
    expect(homeLayout(1).slots[0].side).toBe('R');
    const big = homeLayout(14);
    expect(big.height).toBeGreaterThan(homeLayout(4).height);
    expect(Math.min(...big.slots.map((s) => s.y - 22))).toBeGreaterThanOrEqual(78);
    expect(Math.max(...big.slots.map((s) => s.y + 22))).toBeLessThanOrEqual(big.height);
  });
  it('the viewBox follows the layout height', () => {
    const svg = homeSkeleton({ ...base, loads: Array.from({ length: 8 }, (_, i) => ({ id: 'l' + i, name: 'L' + i, icon: 'plug' as const, v: 0.1 })) });
    expect(svg).toContain('viewBox="0 0 760 ' + homeLayout(8).height + '"');
  });
});

describe('source cards: the text stays inside the card (EN «importing from grid» ran past Grid, 20.10)', () => {
  // Estimates on the safe side: 11 px mono sub ≈ 0.62 em/glyph, 14 px semibold value ≈ 0.6 em.
  const SUB = ['gridNoData', 'gridIdle', 'exportingToGrid', 'importingFromGrid'] as const;
  const VAL = ['noGeneration'] as const;
  it.each([['ru', ruFlow], ['en', enFlow]] as const)('%s', (_lang, d) => {
    const dict = d as Record<string, string>;
    for (const k of SUB) expect(Array.from(dict[k]).length * 11 * 0.62).toBeLessThanOrEqual(SRC_TEXT_W);
    for (const k of VAL) expect(Array.from(dict[k]).length * 14 * 0.6).toBeLessThanOrEqual(SRC_TEXT_W);
  });
});
