import { FlowFrame } from './flowFrame';
import { buildFlowGroups, scopeFrame, ALL_TAB } from './flowGroups';
import { homeSkeleton, homeStructuralKey } from './flowHome';
import { parseGroups } from '../../hooks/useDeviceGroups';

const groups = buildFlowGroups([
  { key: '9020', groupId: 1, name: 'Кухня' },
  { key: '9010', groupId: 1, name: 'Бойлер' },
  { key: '9030', groupId: 2, name: 'ЗС-1' },
  { key: '9031', groupId: 2, name: 'ЗС-2' },
  { key: '9032', groupId: 2, name: 'ЗС-3' },
  { key: '9040', groupId: 3, trunk: true, name: 'Щит гаража' },
  { key: '9041', groupId: 3, name: 'Свет гаража' },
  { key: '9050', name: 'Насос' },
]);

const frame: FlowFrame = {
  grid: 9, solar: null, house: 9, gridHasDirection: true, groups,
  loads: [
    { id: '9020', name: 'Кухня', icon: 'plug', v: 0.6 },
    { id: '9010', name: 'Бойлер', icon: 'drop', v: null },
    { id: '9030', name: 'ЗС-1', icon: 'car', v: 3.3 },
    { id: '9031', name: 'ЗС-2', icon: 'car', v: 0 },
    { id: '9032', name: 'ЗС-3', icon: 'car', v: 1, alarm: true },
    { id: '9040', name: 'Щит гаража', icon: 'plug', v: 1.5 },
    { id: '9041', name: 'Свет гаража', icon: 'bulb', v: 0.2 },
    { id: '9050', name: 'Насос', icon: 'plug', v: 0.4 },
  ],
};

describe('buildFlowGroups (G5 group_id / role_trunk)', () => {
  it('names a group by its trunk, else by ≤ 2 members, else «Группа N»', () => {
    expect(groups.map((g) => g.name)).toEqual(['Кухня + Бойлер', 'Группа 2', 'Щит гаража']);
    expect(groups[2]).toMatchObject({ trunk: '9040', ids: ['9041'] });
  });
  it('the device name (system groups[], e139ab5e) wins over every fallback', () => {
    const names = new Map(parseGroups([{ id: 2, name: 'Парковка' }, { id: 3, name: 'Гараж' }, { id: 0, name: 'x' }, { name: 'no id' }]).map((g) => [g.id, g.name]));
    expect(names.size).toBe(2);
    const named = buildFlowGroups([{ key: 'a', groupId: 2, name: 'ЗС-1' }, { key: 'b', groupId: 3, trunk: true, name: 'Щит' }, { key: 'c', groupId: 3, name: 'Свет' }], names);
    expect(named.map((g) => g.name)).toEqual(['Парковка', 'Гараж']);
  });
});

describe('scopeFrame', () => {
  it('«Все» folds each group into one card (Σ members, or the trunk reading)', () => {
    const f = scopeFrame(frame, ALL_TAB);
    expect(f.loads.map((l) => l.id)).toEqual(['grp:g1', 'grp:g2', 'grp:g3', '9050']);
    expect(f.loads[0]).toMatchObject({ v: 0.6, group: { id: 'g1', count: 2 }, icon: 'layers' });
    expect(f.loads[1]).toMatchObject({ v: 4.3, alarm: true });
    expect(f.loads[2]).toMatchObject({ v: 1.5, group: { id: 'g3', count: 1 } });
  });
  it('a group tab: the group in the hub, the house above, members (+ unmetered rest under a trunk)', () => {
    const g3 = scopeFrame(frame, 'g3');
    expect(g3.scope).toEqual({ id: 'g3', name: 'Щит гаража', house: 9 });
    expect(g3.house).toBe(1.5);
    expect(g3.loads.map((l) => l.id)).toEqual(['9041', '__gother__']);
    expect(g3.loads[1].v).toBeCloseTo(1.3, 5);
    const svg = homeSkeleton(g3);
    expect(svg).toContain('val:up');
    expect(svg).not.toContain('val:grid');
    expect(homeStructuralKey(g3)).not.toBe(homeStructuralKey(scopeFrame(frame, 'g1')));
  });
  it('no groups / unknown tab → unchanged / «Все»', () => {
    const plain = { ...frame, groups: [] };
    expect(scopeFrame(plain, 'g1')).toBe(plain);
    expect(scopeFrame(frame, 'g9').loads[0].id).toBe('grp:g1');
  });
  it('group cards are clickable stacks in the skeleton', () => {
    expect(homeSkeleton(scopeFrame(frame))).toContain('data-group="g2"');
  });
});
