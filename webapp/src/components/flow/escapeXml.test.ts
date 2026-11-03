import { escapeXml } from './escapeXml';
import { FlowFrame } from './flowFrame';
import { homeSkeleton } from './flowHome';
import { sankeySkeleton } from './flowSankey';
import { balanceSkeleton } from './flowBalance';
import { treeSkeleton, TreeData } from './flowTree';

const EVIL = '<img src=x onerror=alert(1)>';
const EVIL_ESC = '&lt;img src=x onerror=alert(1)&gt;';
const ATTR_EVIL = '"><script>alert(1)</script>';

function expectSafe(svg: string): void {
  expect(svg).not.toContain('<img');
  expect(svg).not.toContain('<script');
  expect(svg).toContain(EVIL_ESC);
}

const frame: FlowFrame = {
  grid: 3,
  solar: 1,
  house: 4,
  gridHasDirection: true,
  loads: [
    { id: ATTR_EVIL, name: EVIL, icon: 'plug', v: 2 },
    { id: 'b', name: 'ok', short: EVIL, icon: 'plug', v: 1.5 },
  ],
};

describe('escapeXml', () => {
  it('escapes the five XML specials', () => {
    expect(escapeXml(`&<>"'`)).toBe('&amp;&lt;&gt;&quot;&#39;');
    expect(escapeXml(EVIL)).toBe(EVIL_ESC);
    expect(escapeXml(null)).toBe('');
    expect(escapeXml(42)).toBe('42');
  });
});

describe('flow builders escape data-derived text', () => {
  it('homeSkeleton (names + data-fl ids)', () => {
    const svg = homeSkeleton(frame);
    expectSafe(svg);
    expect(svg).toContain('&quot;&gt;&lt;script&gt;');
  });

  it('sankeySkeleton (labels + <title>)', () => {
    expectSafe(sankeySkeleton(frame));
  });

  it('balanceSkeleton (labels + <title>)', () => {
    expectSafe(balanceSkeleton(frame));
  });

  it('treeSkeleton (group, line name, tag, bus label)', () => {
    const d: TreeData = {
      grid: 2,
      gridHasDirection: false,
      busLabel: EVIL,
      groups: [
        {
          id: 'g1',
          name: EVIL,
          kids: [
            { id: 'k1', name: EVIL, v: 1 },
            { id: 'k2', name: 'ok', label: ATTR_EVIL, v: 0.5 },
          ],
        },
      ],
    };
    const svg = treeSkeleton(d);
    expectSafe(svg);
    expect(svg).toContain('&quot;&gt;&lt;script&gt;');
  });
});
