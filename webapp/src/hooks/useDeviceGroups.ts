/**
 * useDeviceGroups — consumer-group NAMES from the device (root e139ab5e: system-config
 * `groups[] {id, name}`; id > 0, name ≤ schema.limits.name_bytes UTF-8, ≤ 16 groups).
 * POST replaces the whole list (a missing key keeps it), applies live, persists.
 * Membership lives on the units (`group_id` / `role_trunk`, per-unit patch).
 *
 * `supported` = the hub reports the `groups` key (older firmware has no names → the UI keeps
 * its fallbacks: trunk name → «A + B» → «Группа N»).
 */
import { useCallback, useEffect, useMemo } from 'react';
import { useConfigShadow, useDeviceSession } from '../core';
import type { JsonObject } from '../core';

export const MAX_GROUPS = 16;

export interface DeviceGroup { id: number; name: string }

export function parseGroups(v: unknown): DeviceGroup[] {
  if (!Array.isArray(v)) return [];
  return v
    .filter((g): g is { id: number; name: unknown } => !!g && typeof g === 'object' && typeof (g as { id?: unknown }).id === 'number')
    .filter((g) => g.id > 0)
    .map((g) => ({ id: g.id, name: typeof g.name === 'string' ? g.name : '' }));
}

export function useDeviceGroups(): {
  groups: DeviceGroup[];
  names: Map<number, string>;
  supported: boolean;
  save: (next: DeviceGroup[]) => Promise<void>;
} {
  const session = useDeviceSession();
  const sys = useConfigShadow('system');
  useEffect(() => {
    if (Object.keys(session.current.get('system').reported).length === 0) {
      void session.loadDomain('system').catch(() => undefined);
    }
  }, [session]);
  const raw = sys.reported.groups;
  const groups = useMemo(() => parseGroups(raw), [raw]);
  const names = useMemo(() => new Map(groups.filter((g) => g.name).map((g) => [g.id, g.name])), [groups]);
  const save = useCallback(
    (next: DeviceGroup[]) => session.applyLiveField('system', 'groups', next as unknown as JsonObject[string]),
    [session],
  );
  return { groups, names, supported: Array.isArray(raw), save };
}
