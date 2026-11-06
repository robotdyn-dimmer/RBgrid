/**
 * Phase group of a multi-channel rbAmp module (STD-U3I3): all current channels of the module
 * bound as ONE three-phase unit (root delta №5 «уточнения формы bind», frozen 2026-10-15; S2).
 * - bind: POST /api/v1/units with sensor `ch: 254` → unit with channel_idx 254, phase 4 (3P);
 *   per-channel bind ch=c → phase c+1.
 * - sensor-list: a grouped module is ONE entry `{iface:"i2c", module, ch:254, unit_id}`; its
 *   physical channels are not listed as unbound. Channel count = `modules[].n_i`.
 */
import { SensorListEntry, SensorModule } from '../types/sensors';

/** Channel index of a module's phase group. */
export const PHASE_GROUP_CH = 254;

export const isPhaseGroup = (s: Pick<SensorListEntry, 'ch'>): boolean => s.ch === PHASE_GROUP_CH;

/** Channel index of a module's voltage input (the bus unit, canon fc16621e / 5994fe7c). */
export const VOLTAGE_INPUT_CH = 255;

/** A module can be phase-grouped when it measures several phases (≥2 voltage and current channels). */
export const isPhaseModule = (m: SensorModule): boolean => (m.n_v ?? 0) >= 2 && (m.n_i ?? 0) >= 2;

/**
 * The input measured on all three phases of one STD-U3I3 (v1.1.2, operator 03.11 via root): the hub
 * regroups the module under the input's point (`POST /api/v1/units/<input>/rebind {module, ch:254}`,
 * applies after a restart). Binding a NEW point as a group (`POST /units` ch 254) is refused by every
 * hub version — that offer is gone. Per present phase module: `regroup` (hub ≥ 1.1.2), `update` (an
 * older hub, a caption only), nothing while the hub version is unknown or the input is already grouped.
 */
export function inputGroupOffer(
  sensors: SensorListEntry[], modules: SensorModule[], inputHex: string | null, fwOk: boolean | null,
): Map<number, 'regroup' | 'update'> {
  const out = new Map<number, 'regroup' | 'update'>();
  if (!inputHex || fwOk === null) return out;
  const id = inputHex.toUpperCase();
  if (sensors.some((s) => isPhaseGroup(s) && (s.unit_id ?? '').toUpperCase() === id)) return out;
  for (const m of modules) if (m.present && isPhaseModule(m)) out.set(m.module, fwOk ? 'regroup' : 'update');
  return out;
}

/**
 * A channel as printed on the module (operator via root 16.10: current channels are «I0», «I1»… —
 * from 0, as `ch`; phases are «L1»…«L3» — from 1; «V» the voltage input, «L1–L3» a grouped input).
 * The hub's `ch_label` wins; older firmware → derived from `ch`.
 */
export function channelLabel(ch: number | null | undefined, label?: string | null): string {
  if (label) return label;
  if (ch == null) return '—';
  if (ch === PHASE_GROUP_CH) return 'L1–L3';
  if (ch === VOLTAGE_INPUT_CH) return 'V';
  return `I${ch}`;
}

/**
 * How the input is metered on a STD-U3I3 (fw 1.1.5, operator 06.11 via root u3i3-per-phase-points):
 * `grouped` = one point on `L1–L3` (phases netted, e.g. a German saldo meter); `split` = one point per
 * phase, each with its own import/export (the input keeps one phase, `legs` = the other MAIN_SUPPLY
 * points on the module). Modules that don't carry the input are absent from the map.
 */
export interface InputMetering {
  /** `single` = the input on one phase of the module, no other phase bound (a split left half-done). */
  mode: 'grouped' | 'split' | 'single';
  inputCh: number;
  legs: string[];
  /** Phase channels (0..2) of the module that carry no point — bound on a split. */
  freePhases: number[];
  bus?: number;
}

export function inputMetering(
  sensors: SensorListEntry[], modules: SensorModule[], inputHex: string | null, roleOf: (unitId: string) => string | undefined,
): Map<number, InputMetering> {
  const out = new Map<number, InputMetering>();
  if (!inputHex) return out;
  const id = inputHex.toUpperCase();
  const mine = sensors.find((s) => (s.unit_id ?? '').toUpperCase() === id && s.module != null && s.ch != null && s.ch !== VOLTAGE_INPUT_CH);
  if (!mine || mine.module == null) return out;
  const mod = modules.find((m) => m.module === mine.module);
  if (!mod || !isPhaseModule(mod)) return out;
  if (isPhaseGroup(mine)) { out.set(mine.module, { mode: 'grouped', inputCh: PHASE_GROUP_CH, legs: [], freePhases: [], bus: mine.bus }); return out; }
  const onModule = sensors.filter((s) => s.module === mine.module);
  const freePhases = [0, 1, 2].filter((c) => c < (mod.n_i ?? 3) && !onModule.some((s) => s.ch === c && s.unit_id));
  const legs = sensors
    .filter((s) => s.module === mine.module && s.unit_id && (s.unit_id ?? '').toUpperCase() !== id
      && s.ch != null && s.ch >= 0 && s.ch <= 2 && roleOf(s.unit_id) === 'MAIN_SUPPLY')
    .map((s) => s.unit_id as string);
  out.set(mine.module, { mode: legs.length ? 'split' : 'single', inputCh: mine.ch as number, legs, freePhases, bus: mine.bus });
  return out;
}
