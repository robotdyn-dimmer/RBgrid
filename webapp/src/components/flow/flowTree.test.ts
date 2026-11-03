import { treeSkeleton, treeSignature, TreeData } from './flowTree';

const base: TreeData = {
  grid: 5, gridHasDirection: true, groups: [{ id: 'main', name: 'ГРЩ', kids: [{ id: 'a', name: 'Кухня', v: 0.6 }] }],
};

describe('treeSkeleton — generation is a source on the main bus', () => {
  it('draws generation above the bus (val:s0), not as a consumer feeder', () => {
    const svg = treeSkeleton({ ...base, sources: [{ id: 's', name: 'Солнце', v: 0.47, gen: true }] });
    expect(svg).toContain('data-fl="val:s0"');
    expect(svg).not.toContain('data-fl="val:0.1"');
  });
  it('a source appearing / going idle is a structural change', () => {
    const a = treeSignature(base);
    const b = treeSignature({ ...base, sources: [{ id: 's', name: 'Солнце', v: 0.47 }] });
    const c = treeSignature({ ...base, sources: [{ id: 's', name: 'Солнце', v: 0 }] });
    expect(new Set([a, b, c]).size).toBe(3);
  });
});
