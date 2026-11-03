/**
 * useGridSystem — the site's mains system (1P / split-phase / 3P) with its nominals, from
 * system-config `grid` when the hub reports it, else derived from units-config phases and
 * hardware-config `network` (entities/gridSystem; root delta №4, canon bbf06397).
 *
 * `save(kind)` writes `system.grid.system` (live, persisted; "" = not set). The nominals are
 * NOT written here: they live in hardware-config `network.{nominal_voltage, nominal_frequency}`
 * and apply on reboot.
 */
import { useCallback, useEffect, useMemo } from 'react';
import { useConfigShadow, useDeviceSession } from '../core';
import type { JsonObject } from '../core';
import { GridKind, GridSystem, parseGrid } from '../entities/gridSystem';
import { UnitsConfigJoin } from './useUnitsConfig';

export interface GridSystemState extends GridSystem {
  /** The hub reports `system.grid` (canon bbf06397+) → the system can be chosen. */
  supported: boolean;
  save: (kind: GridKind | '') => Promise<void>;
}

const num = (v: unknown): number | undefined => (typeof v === 'number' ? v : undefined);

export function useGridSystem(join: UnitsConfigJoin): GridSystemState {
  const session = useDeviceSession();
  const sys = useConfigShadow('system');
  const hw = useConfigShadow('hardware');
  useEffect(() => {
    for (const d of ['system', 'hardware'] as const) {
      if (Object.keys(session.current.get(d).reported).length === 0) {
        void session.loadDomain(d).catch(() => undefined);
      }
    }
  }, [session]);
  const raw = sys.reported.grid;
  // hardware-config `network.{nominal_voltage, nominal_frequency}`; older shape: flat nominal_frequency.
  const net = hw.reported.network as Record<string, unknown> | undefined;
  const hz = num(net?.nominal_frequency) ?? num(hw.reported.nominal_frequency);
  const hv = num(net?.nominal_voltage);
  const grid = useMemo(
    () => parseGrid(raw, {
      phases: join.phases,
      inputPhase: join.input.phase,
      inputRatedV: join.input.ratedVoltageV,
      hardwareHz: hz,
      hardwareV: hv,
    }),
    [raw, hz, hv, join.phases, join.input.phase, join.input.ratedVoltageV],
  );
  const save = useCallback(
    (kind: GridKind | '') => session.applyLiveField('system', 'grid', { system: kind } as unknown as JsonObject[string]),
    [session],
  );
  const supported = !!raw && typeof raw === 'object' && !Array.isArray(raw);
  return useMemo(() => ({ ...grid, supported, save }), [grid, supported, save]);
}
