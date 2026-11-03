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
