import { fwAtLeast } from './hubVersion';

test('firmware version against a release', () => {
  expect(fwAtLeast('1.1.2', [1, 1, 2])).toBe(true);
  expect(fwAtLeast('v1.1.2-dirty', [1, 1, 2])).toBe(true);
  expect(fwAtLeast('1.2.0', [1, 1, 2])).toBe(true);
  expect(fwAtLeast('1.1.1', [1, 1, 2])).toBe(false);
  expect(fwAtLeast('0.9.9', [1, 1, 2])).toBe(false);
  expect(fwAtLeast(undefined, [1, 1, 2])).toBeNull();
});
