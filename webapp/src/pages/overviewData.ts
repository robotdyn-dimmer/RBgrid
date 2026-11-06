/**
 * Pure builders that adapt normalized telemetry + the units-config join into the shapes the
 * Overview schemas/components consume (TreeData for EnergyFlowTree, StationView[] for the
 * Станции grid, LoadSegment[] for LoadBalanceBar). Classification comes from
 * entities/siteModel so every view agrees on one frame (roles, legacy keys, signed power).
 */
import { UnitEntityGroup, unitMetricValue } from '../entities/model';
import { UnitsConfigJoin, phaseLabel } from '../hooks/useUnitsConfig';
import { GridSystem, parseGrid, legsOfPhase, mainsVoltageText } from '../entities/gridSystem';
import {
  isMainKey, isPseudoKey, isAltKey, isSupplyRole, isGenerationRole, isStationRole,
  signedPowerW, fullPower, fullPowerW, displayName, fullDisplayName, VBUS_KEY,
} from '../entities/siteModel';
import type { TreeData } from '../components/flow/flowTree';
import type { FlowGroup } from '../components/flow/flowFrame';
import { StationView } from '../components/stations/StationCard';
import { LoadSegment } from '../components/stations/LoadBalanceBar';
import { deriveStationStatus, EvThresholds, stationThresholds } from '../components/stations/evStatus';
import { t, tCode } from '../i18n';
import type { UnitRole } from '../types/device';
import { currentOnlyUnitKeys } from '../hooks/useHardware';

const W_PER_KW = 1000;

/** The main input group (legacy "main" / HEX 0006, or the MAIN_SUPPLY current unit). */
export function mainUnit(groups: UnitEntityGroup[], join: UnitsConfigJoin): UnitEntityGroup | undefined {
  return groups.find((g) => isMainKey(g.unitKey))
    ?? groups.find((g) => (join.forGroup(g)?.role ?? g.role) === 'MAIN_SUPPLY' && !isPseudoKey(g.unitKey));
}

/** A line unit = not a supply (main/alt), not a pseudo unit (bus voltage). */
export function lineUnits(groups: UnitEntityGroup[], join: UnitsConfigJoin, currentOnly: Set<string> = currentOnlyUnitKeys()): UnitEntityGroup[] {
  return groups.filter(
    (g) => !isMainKey(g.unitKey) && !isAltKey(g.unitKey) && !isPseudoKey(g.unitKey)
      // current-only modules aren't energy metering (operator 18.10): not a line of the balance
      && !currentOnly.has(g.unitKey.toUpperCase())
      && !isSupplyRole(join.forGroup(g)?.role ?? (g.role as UnitRole | undefined)),
  );
}

/** Generation line: configured RENEWABLE_SOURCE. */
export const isGenerationUnit = (g: UnitEntityGroup, join: UnitsConfigJoin): boolean =>
  isGenerationRole(join.forGroup(g)?.role);

/**
 * An EV charging point: configured role EV_CHARGING; for units without a configured role
 * only the explicit `ev.` point prefix counts (no fuzzy name matching — «Подстанция» is not one).
 */
export function isStation(g: UnitEntityGroup, join: UnitsConfigJoin): boolean {
  const role = join.forGroup(g)?.role;
  if (role) return isStationRole(role);
  return (g.point ?? '').startsWith('ev.');
}

/** Display name for any unit (configured short name → name → point → key). */
/** Short name — schemes, trees, tiles (hub short_name ≤ 10 bytes). */
export const nameOf = (g: UnitEntityGroup, join: UnitsConfigJoin): string => displayName(g, join.forGroup(g));
/** Full name — page headers, tables, alerts, advice. */
export const fullNameOf = (g: UnitEntityGroup, join: UnitsConfigJoin): string => fullDisplayName(g, join.forGroup(g));

/** Предприятие tree: G5 groups as switchboards (+ «ГРЩ» for the ungrouped), generation as a source. */
export function buildIndustrialTree(
  groups: UnitEntityGroup[],
  join: UnitsConfigJoin,
  grid: number,
  gridHasDirection: boolean,
  lost?: boolean,
  flowGroups?: FlowGroup[],
  mains?: GridSystem,
): TreeData {
  const nodes = lineUnits(groups, join).map((g) => nodeFor(g, join));
  // Consumer groups (G5) become the switchboards; ungrouped lines stay under «ГРЩ».
  const loads = nodes.filter((n) => !n.gen);
  const treeGroups: TreeData['groups'] = [];
  for (const fg of flowGroups ?? []) {
    const kids = loads.filter((n) => fg.ids.includes(n.id));
    if (kids.length) treeGroups.push({ id: fg.id, name: fg.name, kids });
  }
  const grouped = new Set(treeGroups.flatMap((g) => g.kids.map((k) => k.id)));
  const rest = loads.filter((n) => !grouped.has(n.id));
  if (rest.length || !treeGroups.length) treeGroups.unshift({ id: 'main', name: t('overview.groupMain'), kids: rest });
  return {
    grid,
    gridHasDirection,
    busLabel: t('overview.busLabelMain', { v: mainsVoltageText(mains ?? parseGrid(undefined, { phases: join.phases, inputPhase: join.input.phase })) }),
    lost,
    groups: treeGroups,
    // Generation is a SOURCE (second infeed on the main bus), not a consumer feeder.
    sources: nodes.filter((n) => n.gen),
  };
}

/** Two-group tree for Станции: stations vs infrastructure. */
export function buildEvTree(
  groups: UnitEntityGroup[],
  join: UnitsConfigJoin,
  grid: number,
  gridHasDirection: boolean,
  lost?: boolean,
): TreeData {
  const lines = lineUnits(groups, join);
  const stations = lines.filter((g) => isStation(g, join)).map((g) => nodeFor(g, join));
  const all = lines.filter((g) => !isStation(g, join)).map((g) => nodeFor(g, join));
  const infra = all.filter((n) => !n.gen);
  const treeGroups = [];
  if (stations.length) treeGroups.push({ id: 'ev', name: t('overview.groupEv'), kids: stations });
  if (infra.length) treeGroups.push({ id: 'infra', name: t('overview.infrastructure'), kids: infra });
  return { grid, gridHasDirection, busLabel: t('overview.busLabelParking'), lost, groups: treeGroups, sources: all.filter((n) => n.gen) };
}

function nodeFor(g: UnitEntityGroup, join: UnitsConfigJoin) {
  const cfg = join.forGroup(g);
  const { w: p, estimated } = fullPower(g, cfg);
  return {
    id: g.unitKey,
    name: nameOf(g, join),
    v: p == null ? null : Math.abs(p) / W_PER_KW,
    gen: isGenerationRole(cfg?.role),
    alarm: g.attributes?.['alarm'] === true,
    // W9: a single-CT multi-phase line shows its ×N estimate, marked «≈».
    label: cfg?.phase ? `${estimated && p != null ? '≈ ' : ''}${phaseLabel(cfg.phase)}` : undefined,
  };
}

/**
 * The site's charging stations. A three-phase charger bound as one point per conductor (a Basic per
 * phase) is ONE station — one card, one segment, one session, not three (content
 * use-cases-persona-facts #3). The hub has no «same charger» field, so the app reads it from a
 * consumer group (G5 `group_id`) whose station points are 2–3 conductors: each on its own phase
 * L1/L2/L3, with the same rated power. Any other group (a car park's «Charging stations» group of
 * separate chargers) keeps one station per point. A group's trunk point measures the whole charger.
 * `keys` = the points whose readings make the station.
 */
export interface StationSet { lead: UnitEntityGroup; keys: string[]; units: UnitEntityGroup[]; groupId?: number }

export function stationSets(groups: UnitEntityGroup[], join: UnitsConfigJoin): StationSet[] {
  const out: StationSet[] = [];
  const byGroup = new Map<number, StationSet>();
  for (const g of lineUnits(groups, join).filter((u) => isStation(u, join))) {
    const cfg = join.forGroup(g);
    const gid = cfg?.groupId;
    if (!gid) { out.push({ lead: g, keys: [g.unitKey], units: [g] }); continue; }
    let set = byGroup.get(gid);
    if (!set) { set = { lead: g, keys: [], units: [], groupId: gid }; byGroup.set(gid, set); out.push(set); }
    set.units.push(g);
  }
  const singles = (set: StationSet): StationSet[] => set.units.map((u) => ({ lead: u, keys: [u.unitKey], units: [u] }));
  return out.flatMap((set) => {
    if (set.groupId == null) return [set];
    const trunk = set.units.find((u) => join.forGroup(u)?.trunk);
    const parts = set.units.filter((u) => u !== trunk);
    const cfgs = parts.map((u) => join.forGroup(u));
    const phases = new Set(cfgs.map((c) => c?.phase));
    const oneCharger = parts.length >= 2 && parts.length <= 3
      && cfgs.every((c) => !!c?.phase && c.phase >= 1 && c.phase <= 3 && !c.phaseGroup) && phases.size === parts.length
      && new Set(cfgs.map((c) => c?.ratedPowerW ?? 0)).size === 1;
    if (!oneCharger) return singles(set);
    const units = trunk ? [trunk] : parts;
    return [{ lead: units[0], keys: units.map((u) => u.unitKey), units, groupId: set.groupId }];
  });
}

const sumOrNull = (vs: (number | null | undefined)[]): number | null =>
  vs.some((v) => v != null) ? vs.reduce<number>((a, v) => a + (v ?? 0), 0) : null;

/** A station set's name: the device's group name → the lead point's name (its members share a charger). */
const stationName = (set: StationSet, join: UnitsConfigJoin, groupNames?: Map<number, string>): string =>
  (set.groupId != null && set.units.length > 1 ? groupNames?.get(set.groupId) : undefined) || nameOf(set.lead, join);

/** Station cards for the Станции grid. Energy on the wire is Wh → kWh for the card. */
export function buildStations(
  groups: UnitEntityGroup[],
  join: UnitsConfigJoin,
  ev: EvThresholds,
  groupNames?: Map<number, string>,
): StationView[] {
  return stationSets(groups, join).map((set) => {
    const powers = set.units.map((g) => fullPower(g, join.forGroup(g)));
    const p = sumOrNull(powers.map((x) => (x.w == null ? null : Math.abs(x.w))));
    const present = set.units.every((g) => g.present);
    const dayWh = sumOrNull(set.units.map((g) => unitMetricValue(g, 'energy_day_in')));
    const rated = set.units.map((g) => join.forGroup(g)?.ratedPowerW);
    return {
      id: set.lead.unitKey,
      name: stationName(set, join, groupNames),
      powerW: present ? p : null,
      present,
      // Session start isn't tracked until the hub sessions journal (G8) → no «Ожидает» yet.
      status: deriveStationStatus(present ? p : null, present, null, stationThresholds(ev, join.forGroup(set.lead))),
      sessionKwh: dayWh == null ? null : dayWh / W_PER_KW,
      maxPowerW: rated.every((r) => r != null) ? rated.reduce<number>((a, r) => a + (r ?? 0), 0) : null,
      estimated: powers.some((x) => x.estimated),
    };
  });
}

/** Load-balance segments: infra (aggregate consumption) + one per station. */
export function buildLoadSegments(groups: UnitEntityGroup[], join: UnitsConfigJoin, groupNames?: Map<number, string>): LoadSegment[] {
  const lines = lineUnits(groups, join);
  const infra = lines.filter((g) => !isStation(g, join) && !isGenerationUnit(g, join));
  const segs: LoadSegment[] = [];
  const infraW = infra.reduce((s, g) => s + Math.max(0, fullPowerW(g, join.forGroup(g)) ?? 0), 0);
  if (infraW > 0) segs.push({ id: 'infra', name: t('overview.infrastructure'), valueW: infraW, kind: 'infra' });
  for (const set of stationSets(groups, join)) {
    segs.push({
      id: set.lead.unitKey,
      name: stationName(set, join, groupNames),
      valueW: set.units.reduce((a, g) => a + Math.abs(fullPowerW(g, join.forGroup(g)) ?? 0), 0),
      kind: 'station',
    });
  }
  return segs;
}

/** Count of stations charging / total / free. */
export function stationCounts(stations: StationView[]): { charging: number; total: number; free: number } {
  return {
    charging: stations.filter((s) => s.status === 'charging').length,
    free: stations.filter((s) => s.status === 'free').length,
    total: stations.length,
  };
}

/** All real measurement points (everything except pseudo units such as the voltage bus). */
export function pointUnits(groups: UnitEntityGroup[]): UnitEntityGroup[] {
  return groups.filter((g) => !isPseudoKey(g.unitKey));
}

/** A point is «на связи» when present; pseudo units are never counted, nor points whose channel
 *  the fitted module lacks (nothing to answer — `noChannel`). */
export function offlinePoints(groups: UnitEntityGroup[]): UnitEntityGroup[] {
  return pointUnits(groups).filter((g) => !g.present && !g.noChannel);
}

/**
 * Unmetered load «Остаток» = input − Σ lines (signed, generation reduces the sum; full power,
 * a single-CT multi-phase line ×N — W9). null when
 * the input isn't measured. ONE definition for the Предприятие hero and anything else.
 */
export function unmeteredW(groups: UnitEntityGroup[], join: UnitsConfigJoin, inputPowerW: number | null): number | null {
  if (inputPowerW == null) return null;
  const lines = lineUnits(groups, join);
  // A line without data makes the remainder UNKNOWN, not «everything is unmetered» (rerun 16.10
  // INP-R05: .110 with four offline lines showed «Неучтённое 100 % ввода»).
  if (lines.some((g) => !g.present)) return null;
  const sum = lines.reduce((s, g) => s + (fullPowerW(g, join.forGroup(g)) ?? 0), 0);
  return inputPowerW - sum;
}

/** Whether the remainder is worth showing (> 50 W and > 5 % of the input). */
export const isMaterialRemainder = (remW: number | null, inputW: number | null): boolean =>
  remW != null && inputW != null && Math.abs(remW) > Math.max(50, 0.05 * Math.abs(inputW));

export interface PhaseLoad {
  /** Phase / leg number, 1-based (L1..L3; a split-phase service has legs 1 and 2). */
  phase: number;
  /** Σ signed P of the phase's lines, floored at 0 (W); measured: the phase's signed P. */
  powerW: number;
  /** Estimated phase current (A) — signed Σ of the lines' currents, floored at 0. */
  currentA: number;
  /** Lines assigned to the phase (a multi-phase line counts on each of its phases). */
  lines: number;
  hasData: boolean;
  /** MEASURED on the input (U3I3 `ph[]`), not estimated from lines. */
  measured?: boolean;
  /** Measured: the phase reported no P (the field is omitted) → show «—», powerW is 0. */
  noPower?: boolean;
  /** Measured L-N voltage, V. */
  voltageV?: number;
  /** Measured power factor. */
  pf?: number;
  /** Measured reactive power, var (Q > 0 inductive, Q < 0 capacitive). */
  q?: number;
  /** Measured voltage angle vs L1, degrees. */
  angle?: number;
}

/**
 * Per-phase loads MEASURED on the input: the MAIN_SUPPLY unit's `ph[]` (U3I3, root delta №4).
 * Rows = the mains system's legs (when declared) ∪ the reported phases; a leg the unit didn't
 * report has no data. null when the input carries no `ph[]` this frame (normal — not an error).
 */
export function measuredPhaseLoads(groups: UnitEntityGroup[], join: UnitsConfigJoin, grid?: GridSystem): PhaseLoad[] | null {
  // ph[] is its own freshness signal (the firmware drops a snapshot older than 10 s), so it is
  // used even when the input's own `present` is false (a fixture-fed legacy main on .139).
  const ph = mainUnit(groups, join)?.phases;
  if (!ph?.length) return null;
  const gridV = gridPhaseVoltages(groups);
  const legs = Array.from(new Set([...(grid?.legs ?? []), ...ph.map((r) => r.ph)])).sort((a, b) => a - b);
  return legs.map((l) => {
    const r = ph.find((x) => x.ph === l);
    const hasData = r?.i != null || r?.p != null;
    return {
      phase: l,
      powerW: r?.p ?? 0,
      currentA: Math.abs(r?.i ?? 0),
      lines: 0,
      hasData,
      measured: true,
      noPower: r?.p == null,
      // A current-only input leg takes the grid voltage the voltage modules measure on that phase.
      voltageV: r?.v ?? gridV.find((x) => x.ph === l)?.v,
      pf: r?.pf,
      q: r?.q,
      angle: r?.angle,
    };
  });
}

/**
 * Grid voltage per phase, measured by the voltage modules (canon fc16621e: the bus unit's
 * `ph[]` = [{ph, v}], U3I3 → L1..L3, UI1 → one phase). Empty when the bus is absent or carries
 * no `ph[]` (a single L1 phase is just its top-level `v`). Phases are this frame only.
 */
export function gridPhaseVoltages(groups: UnitEntityGroup[]): { ph: number; v: number }[] {
  const bus = groups.find((g) => g.unitKey === VBUS_KEY);
  if (!bus?.present) return [];
  return (bus.phases ?? []).filter((r): r is typeof r & { v: number } => r.v != null).map((r) => ({ ph: r.ph, v: r.v }));
}

/**
 * Share of the input's power that the metered lines account for (0..1+), or null without a
 * measured input. An estimate of phases from lines is only as good as this (UX audit INP-07).
 */
export function linesCoverage(groups: UnitEntityGroup[], join: UnitsConfigJoin): number | null {
  const main = mainUnit(groups, join);
  if (!main?.present) return null;
  const inputW = Math.abs(fullPowerW(main, join.forGroup(main)) ?? 0);
  if (inputW < 50) return null;
  const linesW = lineUnits(groups, join).reduce((s, g) => s + Math.abs(fullPowerW(g, join.forGroup(g)) ?? 0), 0);
  return linesW / inputW;
}

/**
 * Phase current unbalance, % = max |I − Ī| / Ī (NEMA definition) over the MEASURED phases with a
 * current. null for an estimate (meaningless there), fewer than two phases, or Ī < 1 A (noise).
 */
export function phaseImbalancePct(loads: PhaseLoad[] | null): number | null {
  const cur = (loads ?? []).filter((p) => p.measured && p.hasData).map((p) => p.currentA);
  if (cur.length < 2) return null;
  const mean = cur.reduce((s, x) => s + x, 0) / cur.length;
  if (mean < 1) return null;
  return (Math.max(...cur.map((x) => Math.abs(x - mean))) / mean) * 100;
}

/**
 * Per-phase load: MEASURED on the input when it carries `ph[]` (U3I3 — measuredPhaseLoads),
 * else AGGREGATED from the lines assigned to each phase (units-config `phase`, G6). Generation
 * on a phase reduces its load. The phases are the mains system's legs (`grid`; absent → derived from
 * the points' phases, as everywhere — 1P unless something sits on L2/L3).
 * A single-CT unit's readings are those of ONE conductor (root delta №4: firmware neither
 * multiplies nor divides). So a multi-phase line — 3P (4) or two-pole L1L2/L2L3/L3L1 (5–7) —
 * adds its measured P and I to EACH of its phases: exact for a two-pole load's current, a
 * balance estimate otherwise (the card is labelled «оценка по линиям»).
 * null when no line carries a phase assignment.
 */
export function phaseLoads(groups: UnitEntityGroup[], join: UnitsConfigJoin, grid?: GridSystem): PhaseLoad[] | null {
  // A metered input (U3I3 ph[]) beats the estimate from lines.
  const measured = measuredPhaseLoads(groups, join, grid);
  if (measured) return measured;
  // One default with the rest of the app (deriveGridKind): the system the points' phases imply —
  // a site with nothing on L2/L3 is single-phase, never a three-phase desk by default.
  const sys = grid ?? parseGrid(undefined, { phases: join.phases, inputPhase: join.input.phase });
  const agg = new Map<number, PhaseLoad>(sys.legs.map((l) => [l, { phase: l, powerW: 0, currentA: 0, lines: 0, hasData: false }]));
  let anyAssigned = false;
  for (const g of lineUnits(groups, join)) {
    const ph = join.forGroup(g)?.phase;
    const targets = legsOfPhase(ph, sys);
    if (!targets.length) continue;
    anyAssigned = true;
    for (const l of targets) agg.get(l)!.lines += 1;
    const p = signedPowerW(g);
    if (p == null) continue;
    const i = Math.abs(unitMetricValue(g, 'current') ?? 0);
    for (const l of targets) {
      const a = agg.get(l)!;
      a.powerW += p;
      a.currentA += (p < 0 ? -1 : 1) * i;
      a.hasData = true;
    }
  }
  if (!anyAssigned) return null;
  // A phase where generation outweighs the loads EXPORTS — keep the sign (shown «↑ …», UX audit
  // INP-16: solar on L2 used to read «0 Вт · 0,0 А»); the current is its magnitude.
  return sys.legs.map((l) => {
    const a = agg.get(l)!;
    return { ...a, currentA: Math.abs(a.currentA) };
  });
}

/** Firmware role enum (schema enums.role) → phrase from the `role` file; unknown → the code. */
export const roleLabel = (role: string | null | undefined): string => tCode('role', role);

/** Role label for a point: configured role → legacy key / EV prefix fallback → «—». */
export function roleLabelOf(g: UnitEntityGroup, join: UnitsConfigJoin): string {
  const role = join.forGroup(g)?.role;
  if (role) return roleLabel(role);
  // Fallbacks reuse the role phrases (same wording as the configured roles).
  if (isMainKey(g.unitKey)) return roleLabel('MAIN_SUPPLY');
  if (isAltKey(g.unitKey)) return roleLabel('ALTERNATIVE_SUPPLY');
  if (isStation(g, join)) return roleLabel('EV_CHARGING');
  return t('common.dash');
}
