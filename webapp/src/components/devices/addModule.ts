/**
 * Adding an rbAmp module on bus 0 (v1.1.2, operator 02.11 via root). Every module leaves the
 * factory at 0x50, and the hub adds only a module whose address no configured module holds.
 * So before a new module goes on the bus, the module the hub has at 0x50 moves to a free address
 * (UID-checked, its points keep measuring) — then the new one answers alone at 0x50 and is added.
 * Moving the NEW module instead (connect it alone, readdress it) would make the hub take it for
 * the absent module at 0x50 and hand that module's points to it.
 *
 * The step is derived from the hub's state, not remembered, so the dialog can be closed for a
 * power-off and picked up again.
 */
import type { I2cDevice, I2cState } from '../../types/i2c';

export const FACTORY_ADDR = 0x50;
/** The rbAmp address range (the hub identifies modules only there). */
export const RBAMP_FIRST = 0x50;
export const RBAMP_LAST = 0x5f;
/** Bus 0 only in v1.1.x (the second bus is out of scope). */
export const BUS = 0;
const DEFAULT_MAX = 3;

export const hex = (a: number): string => `0x${a.toString(16).toUpperCase().padStart(2, '0')}`;

export const maxModules = (s: I2cState): number => (typeof s.max_modules === 'number' && s.max_modules > 0 ? s.max_modules : DEFAULT_MAX);

/** The configured module that holds the factory address on bus 0, if any. */
export const atFactory = (s: I2cState) => s.modules.find((m) => m.bus === BUS && m.addr === FACTORY_ADDR);

/** First address after 0x50 that no configured module and no answering device on bus 0 uses. */
export function nextFreeAddr(s: I2cState, seen: I2cDevice[] = []): number | null {
  const used = new Set([...s.modules.filter((m) => m.bus === BUS).map((m) => m.addr), ...seen.map((d) => d.addr)]);
  for (let a = FACTORY_ADDR + 1; a <= RBAMP_LAST; a++) if (!used.has(a)) return a;
  return null;
}

/** rbAmp modules a scan found that the hub doesn't have yet. */
export const newModules = (devices: I2cDevice[]): I2cDevice[] => devices.filter((d) => d.kind === 'rbamp' && d.module == null);

/** Devices that answered but are not rbAmp modules (named so a stray address isn't a mystery). */
export const otherDevices = (devices: I2cDevice[]): I2cDevice[] => devices.filter((d) => d.kind !== 'rbamp');

export type AddStep =
  /** The hub already has its maximum. */
  | { kind: 'full'; max: number }
  /** Free 0x50 first: move this module to `to` (it must be answering — the hub checks its UID). */
  | { kind: 'free'; module: number; to: number | null; present: boolean; model?: string }
  /** 0x50 is free: connect the new module, then scan. */
  | { kind: 'connect'; count: number; max: number }
  /** v1.1.4: the slot at 0x50 is an empty placeholder — a module from the box connected there simply
   *  becomes that module (the hub identifies it at start-up); no move, no add. */
  | { kind: 'adopt'; module: number };

export function addStep(s: I2cState): AddStep {
  const max = maxModules(s);
  if (s.modules.length >= max) return { kind: 'full', max };
  const f = atFactory(s);
  if (f?.placeholder === true) return { kind: 'adopt', module: f.module };
  if (f) return { kind: 'free', module: f.module, to: nextFreeAddr(s), present: f.present, model: f.model };
  return { kind: 'connect', count: s.modules.length, max };
}

/**
 * Wiring advice once the bus carries more than one module (count AFTER adding). rbamp-dev 03.11 via
 * root, cables up to 15 cm: up to three modules keep their on-board pull-ups as they are (3 modules,
 * all on, 15 cm, 50 kHz: 0 failures in 11 253 reads); from the 4th (v1.1.2 takes up to 8) all but one
 * module's pull-ups go, or one external pair — the rbAmp guide's table (calculated, not measured).
 */
export const pullupKey = (after: number): 'addmod.pullup' | 'addmod.pullup4' | null =>
  after >= 4 ? 'addmod.pullup4' : after >= 2 ? 'addmod.pullup' : null;
