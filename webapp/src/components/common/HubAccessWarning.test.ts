import { accessLevel } from './HubAccessWarning';

test('hub access warning: full while neither is done, lighter while one is', () => {
  expect(accessLevel(false, false)).toBe('full');
  expect(accessLevel(false, null)).toBe('full'); // no token → the hub won't show the AP status
  expect(accessLevel(false, true)).toBe('token');
  expect(accessLevel(true, false)).toBe('ap');
  expect(accessLevel(true, true)).toBeNull();
  expect(accessLevel(true, null)).toBeNull();
  expect(accessLevel(null, false)).toBeNull();
});
