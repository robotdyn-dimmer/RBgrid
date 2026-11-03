/**
 * Consumer groups for the flow schemas (G5: units-config `group_id` + `role_trunk`; names in
 * system-config `groups[]`, root e139ab5e). A trunk measures its group: members are its breakdown
 * and never add on top of it (root canon).
 *
 *  - {@link buildFlowGroups}: units-config join → FlowGroup[]. A group's name: the device's
 *    system `groups[]` name (e139ab5e) → its trunk meter's name → its members' names when there
 *    are two at most («Кухня + Бойлер») → «Группа N».
 *  - {@link scopeFrame}: the frame a tab shows. «Все» folds every group into ONE card (the
 *    trunk's reading, else Σ members); a group tab puts the group in the hub, the house above
 *    it, and the members (+ «Прочее в группе» = trunk − Σ members) around it.
 * Pure; the renderers keep their no-flicker contract (the tab is part of the structural key).
 */
import { FlowFrame, FlowGroup, FlowLoad } from './flowFrame';
import { t } from '../../i18n';

/** Tab id of the whole-house view. */
export const ALL_TAB = 'all';
const OTHER_MIN_W = 50;

export interface GroupMember {
  /** Telemetry key (FlowLoad id). */
  key: string;
  groupId?: number;
  trunk?: boolean;
  name: string;
}

export function buildFlowGroups(members: GroupMember[], names?: Map<number, string>): FlowGroup[] {
  const by = new Map<number, { ids: string[]; names: string[]; trunk?: string; trunkName?: string }>();
  for (const m of members) {
    if (!m.groupId) continue;
    const g = by.get(m.groupId) ?? { ids: [], names: [] };
    if (m.trunk && !g.trunk) { g.trunk = m.key; g.trunkName = m.name; } else { g.ids.push(m.key); g.names.push(m.name); }
    by.set(m.groupId, g);
  }
  return Array.from(by.entries())
    .filter(([, g]) => g.ids.length > 0)
    .sort(([a], [b]) => a - b)
    .map(([id, g]) => ({
      id: `g${id}`,
      name: names?.get(id) || g.trunkName || (g.names.length <= 2 ? g.names.join(' + ') : t('flow.groupN', { n: id })),
      ids: g.ids,
      trunk: g.trunk,
    }));
}

const sumKw = (ls: FlowLoad[]): number | null =>
  ls.some((l) => l.v != null) ? ls.reduce((s, l) => s + (l.v ?? 0), 0) : null;

/** The frame for a tab; unknown tab / no groups → the «Все» view. */
export function scopeFrame(f: FlowFrame, tab: string = ALL_TAB): FlowFrame {
  const groups = f.groups ?? [];
  if (!groups.length) return f;
  const groupOf = new Map<string, FlowGroup>();
  groups.forEach((g) => { g.ids.forEach((id) => groupOf.set(id, g)); if (g.trunk) groupOf.set(g.trunk, g); });

  const g = groups.find((x) => x.id === tab);
  if (g) {
    const members = f.loads.filter((l) => g.ids.includes(l.id));
    const trunk = g.trunk ? f.loads.find((l) => l.id === g.trunk) : undefined;
    const sum = sumKw(members);
    const total = trunk?.v ?? sum ?? 0;
    const loads = [...members];
    const rest = trunk?.v != null ? trunk.v - (sum ?? 0) : 0;
    if (rest * 1000 > OTHER_MIN_W && rest > total * 0.05) {
      loads.push({ id: '__gother__', name: t('flow.groupOther'), icon: 'plug', v: rest, synthetic: true });
    }
    return { ...f, loads, house: total, scope: { id: g.id, name: g.name, house: f.house } };
  }

  // «Все»: each group becomes one card at the place of its first member.
  const loads: FlowLoad[] = [];
  const done = new Set<string>();
  for (const l of f.loads) {
    const grp = groupOf.get(l.id);
    if (!grp) { loads.push(l); continue; }
    if (done.has(grp.id)) continue;
    done.add(grp.id);
    const members = f.loads.filter((x) => grp.ids.includes(x.id));
    const trunk = grp.trunk ? f.loads.find((x) => x.id === grp.trunk) : undefined;
    loads.push({
      id: `grp:${grp.id}`,
      name: grp.name,
      icon: 'layers',
      v: trunk ? trunk.v : sumKw(members),
      alarm: members.some((m) => m.alarm) || !!trunk?.alarm,
      group: { id: grp.id, count: members.length },
    });
  }
  return { ...f, loads };
}
