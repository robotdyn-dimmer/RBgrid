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
