import { computeDelta, deepEqual, deltaPaths } from '../json';
import { ConfigShadow, DeviceShadow } from './ConfigShadow';

describe('computeDelta (AWS-IoT shadow semantics)', () => {
  it('empty when reported equals desired', () => {
    expect(computeDelta({ a: 1, b: 'x' }, { a: 1, b: 'x' })).toEqual({});
  });

  it('includes only changed top-level keys', () => {
    expect(computeDelta({ a: 1, b: 2 }, { a: 1, b: 3 })).toEqual({ b: 3 });
  });

  it('reported-only keys are NOT in the delta (desired is the intent)', () => {
    expect(computeDelta({ a: 1, b: 2 }, { a: 1 })).toEqual({});
  });

  it('desired-only keys appear in the delta', () => {
    expect(computeDelta({ a: 1 }, { a: 1, c: 9 })).toEqual({ c: 9 });
  });

  it('recurses nested objects, dropping unchanged siblings', () => {
    const reported = { net: { ssid: 'a', rssi: -50 }, name: 'dev' };
    const desired = { net: { ssid: 'b', rssi: -50 }, name: 'dev' };
    expect(computeDelta(reported, desired)).toEqual({ net: { ssid: 'b' } });
  });

  it('replaces arrays wholesale on any mismatch', () => {
    expect(computeDelta({ zones: [1, 2] }, { zones: [1, 3] })).toEqual({ zones: [1, 3] });
  });
});

describe('deepEqual', () => {
  it('handles nested objects and arrays', () => {
    expect(deepEqual({ a: [1, { b: 2 }] }, { a: [1, { b: 2 }] })).toBe(true);
    expect(deepEqual({ a: [1, { b: 2 }] }, { a: [1, { b: 3 }] })).toBe(false);
  });
  it('distinguishes null from missing/0', () => {
    expect(deepEqual(null, 0)).toBe(false);
    expect(deepEqual(null, null)).toBe(true);
  });
});

describe('deltaPaths', () => {
  it('flattens nested deltas to dotted leaf paths', () => {
    expect(deltaPaths({ net: { ssid: 'b' }, name: 'x' }).sort()).toEqual(['name', 'net.ssid']);
  });
});

describe('ConfigShadow', () => {
  it('reports no changes when desired tracks reported', () => {
    const s = new ConfigShadow('system', { home_name: 'Home' });
    expect(s.hasChanges).toBe(false);
    expect(s.delta).toEqual({});
  });

  it('computes delta + changedPaths after an edit', () => {
    const s = new ConfigShadow('system', { home_name: 'Home', language: 'EN' })
      .withDesired('home_name', 'Lab', 1000);
    expect(s.hasChanges).toBe(true);
    expect(s.delta).toEqual({ home_name: 'Lab' });
    expect(s.changedPaths).toEqual(['home_name']);
    expect(s.metadata.home_name).toBe(1000);
  });

  it('flags reboot-requiring domains only when dirty', () => {
    const clean = new ConfigShadow('wifi', { ssid: 'a' });
    expect(clean.requiresReboot).toBe(false);
    const dirty = clean.withDesired('ssid', 'b', 1);
    expect(dirty.requiresReboot).toBe(true);
    const sysDirty = new ConfigShadow('system', { x: 1 }).withDesired('x', 2, 1);
    expect(sysDirty.requiresReboot).toBe(false); // system isn't a reboot domain
  });

  it('preserves user intent across a reported refresh, bumps version', () => {
    const edited = new ConfigShadow('system', { home_name: 'Home' }).withDesired('home_name', 'Lab', 1);
    const refreshed = edited.withReported({ home_name: 'Home' });
    expect(refreshed.version).toBe(1);
    expect(refreshed.desired.home_name).toBe('Lab'); // edit survives
    expect(refreshed.hasChanges).toBe(true);
  });

  it('tracks reported when there are no pending edits', () => {
    const clean = new ConfigShadow('system', { home_name: 'Home' }, undefined, 5);
    const refreshed = clean.withReported({ home_name: 'Cabin' });
    expect(refreshed.desired.home_name).toBe('Cabin'); // no edits → follow device
    expect(refreshed.hasChanges).toBe(false);
  });

  it('discards stale reported updates (older version)', () => {
    const s = new ConfigShadow('system', { v: 'new' }, undefined, 12);
    const stale = s.withReported({ v: 'old' }, 11);
    expect(stale).toBe(s); // unchanged
    expect(stale.reported.v).toBe('new');
  });

  it('reset drops edits back to reported', () => {
    const s = new ConfigShadow('system', { a: 1 }).withDesired('a', 2, 1).reset();
    expect(s.hasChanges).toBe(false);
    expect(s.desired.a).toBe(1);
  });
});

describe('DeviceShadow', () => {
  it('aggregates dirty domains and reboot flag', () => {
    let dev = new DeviceShadow();
    dev = dev.set('system', new ConfigShadow('system', { a: 1 }).withDesired('a', 2, 1));
    dev = dev.set('wifi', new ConfigShadow('wifi', { ssid: 'x' }));
    expect(dev.dirtyDomains).toEqual(['system']);
    expect(dev.requiresReboot).toBe(false);
    dev = dev.set('wifi', dev.get('wifi').withDesired('ssid', 'y', 1));
    expect(dev.dirtyDomains.sort()).toEqual(['system', 'wifi']);
    expect(dev.requiresReboot).toBe(true); // wifi is a reboot domain
  });
});
