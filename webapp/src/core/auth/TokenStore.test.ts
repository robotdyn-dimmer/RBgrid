import { TokenStore, KeyValueStorage } from './TokenStore';

class FakeStorage implements KeyValueStorage {
  map = new Map<string, string>();
  getItem(k: string) { return this.map.has(k) ? this.map.get(k)! : null; }
  setItem(k: string, v: string) { this.map.set(k, v); }
  removeItem(k: string) { this.map.delete(k); }
}

describe('TokenStore', () => {
  it('starts empty with no storage', () => {
    const t = new TokenStore();
    expect(t.has()).toBe(false);
    expect(t.get()).toBeNull();
    expect(t.authHeader()).toEqual({});
  });

  it('set/get/clear with the Bearer header', () => {
    const t = new TokenStore();
    t.set('abc123');
    expect(t.has()).toBe(true);
    expect(t.get()).toBe('abc123');
    expect(t.authHeader()).toEqual({ Authorization: 'Bearer abc123' });
    t.clear();
    expect(t.has()).toBe(false);
    expect(t.authHeader()).toEqual({});
  });

  it('persists to + rehydrates from storage', () => {
    const store = new FakeStorage();
    new TokenStore(store).set('persisted-token');
    expect(store.map.get('rbgrid.token')).toBe('persisted-token');
    const reloaded = new TokenStore(store);
    expect(reloaded.get()).toBe('persisted-token'); // survives "reload"
  });

  it('clear removes from storage', () => {
    const store = new FakeStorage();
    const t = new TokenStore(store);
    t.set('x');
    t.clear();
    expect(store.map.has('rbgrid.token')).toBe(false);
  });

  it('treats empty string as no token', () => {
    const t = new TokenStore();
    t.set('');
    expect(t.has()).toBe(false);
  });
});

import { tokenScopeOf, migrateLegacyToken } from './TokenStore';

/** Kimi #1: one hub's token never goes to another hub (a device switch or a ?device= link). */
describe('TokenStore — one slot per hub', () => {
  it('a token saved for hub A is not sent to hub B', () => {
    const store = new FakeStorage();
    let target = 'http://192.168.1.139';
    const t = new TokenStore(store, undefined, () => tokenScopeOf(target));
    t.set('a-token');
    target = 'https://attacker.example';
    expect(t.authHeader()).toEqual({});
    target = 'http://192.168.1.139/';
    expect(t.authHeader()).toEqual({ Authorization: 'Bearer a-token' });
  });
  it('same-origin (served by the hub) scopes to the page origin', () => {
    expect(tokenScopeOf('', 'http://rbgrid-1a2b.local/#/settings')).toBe('http://rbgrid-1a2b.local');
  });
  it('the old global token moves to the persisted hub only, never to a deep link', () => {
    const ls = new FakeStorage();
    ls.setItem('rbgrid.token', 'old');
    ls.setItem('rbgrid.connection', JSON.stringify({ baseUrl: 'http://192.168.1.139', demo: false }));
    migrateLegacyToken(ls);
    expect(ls.getItem('rbgrid.token')).toBeNull();
    expect(ls.getItem('rbgrid.token@http://192.168.1.139')).toBe('old');
  });
  it('without a persisted hub the old token is dropped', () => {
    const ls = new FakeStorage();
    ls.setItem('rbgrid.token', 'old');
    migrateLegacyToken(ls);
    expect(ls.map.size).toBe(0);
  });
});
