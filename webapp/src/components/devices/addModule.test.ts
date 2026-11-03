import { addStep, nextFreeAddr, newModules, otherDevices, pullupKey, hex } from './addModule';
import type { I2cState } from '../../types/i2c';

const mod = (module: number, addr: number, present = true) => ({ module, bus: 0, addr, present });
const st = (modules: ReturnType<typeof mod>[], max = 3): I2cState => ({ buses: [{ bus: 0, up: true }], modules, max_modules: max });

describe('add an rbAmp module', () => {
  it('frees 0x50 first: the module there moves to the next free address', () => {
    expect(addStep(st([mod(0, 0x50)]))).toEqual({ kind: 'free', module: 0, to: 0x51, present: true, model: undefined });
    expect(addStep(st([mod(0, 0x51), mod(1, 0x50)]))).toMatchObject({ kind: 'free', module: 1, to: 0x52 });
  });
  it('an absent module at 0x50 cannot be moved', () => {
    expect(addStep(st([mod(0, 0x50, false)]))).toMatchObject({ kind: 'free', present: false });
  });
  it('0x50 free → connect; at the limit → full', () => {
    expect(addStep(st([mod(0, 0x51)]))).toEqual({ kind: 'connect', count: 1, max: 3 });
    expect(addStep(st([mod(0, 0x51), mod(1, 0x52), mod(2, 0x50)]))).toEqual({ kind: 'full', max: 3 });
    expect(addStep({ buses: [], modules: [mod(0, 0x51), mod(1, 0x52), mod(2, 0x53)] })).toEqual({ kind: 'full', max: 3 });
  });
  it('v1.1.2 takes up to 8: the 8th module still finds 0x50 freed to 0x57', () => {
    const seven = Array.from({ length: 6 }, (_, i) => mod(i, 0x51 + i)).concat(mod(6, 0x50));
    expect(addStep(st(seven, 8))).toMatchObject({ kind: 'free', module: 6, to: 0x57 });
    expect(addStep(st(seven.map((m, i) => (i === 6 ? mod(6, 0x57) : m)), 8))).toEqual({ kind: 'connect', count: 7, max: 8 });
  });
  it('a free address skips configured modules and devices that answered', () => {
    expect(nextFreeAddr(st([mod(0, 0x50), mod(1, 0x51)]), [{ addr: 0x52, module: null, kind: 'unknown' }])).toBe(0x53);
    expect(nextFreeAddr(st(Array.from({ length: 16 }, (_, i) => mod(i, 0x50 + i)), 20))).toBeNull();
  });
  it('a scan splits new rbAmp modules from the rest', () => {
    const d = [
      { addr: 0x50, module: null, kind: 'rbamp' as const, fw: 'STD-1.7.2' },
      { addr: 0x51, module: 0, kind: 'rbamp' as const },
      { addr: 0x27, module: null, kind: 'device' as const },
    ];
    expect(newModules(d).map((x) => x.addr)).toEqual([0x50]);
    expect(otherDevices(d).map((x) => hex(x.addr))).toEqual(['0x27']);
  });
  it('pull-up advice by the count after adding', () => {
    expect(pullupKey(1)).toBeNull();
    expect(pullupKey(2)).toBe('addmod.pullup');
    expect(pullupKey(3)).toBe('addmod.pullup');
    expect(pullupKey(4)).toBe('addmod.pullup4');
    expect(pullupKey(8)).toBe('addmod.pullup4');
  });
});
