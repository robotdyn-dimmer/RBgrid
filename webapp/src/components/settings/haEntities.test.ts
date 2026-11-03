import { fixedMoneyEntities } from './HomeAssistantPanel';

test('Export value entity from firmware 1.1.2 on', () => {
  expect(fixedMoneyEntities('1.1.1')).toBe(false);
  expect(fixedMoneyEntities('v1.1.0')).toBe(false);
  expect(fixedMoneyEntities('1.1.2')).toBe(true);
  expect(fixedMoneyEntities('v1.1.2-dirty')).toBe(true);
  expect(fixedMoneyEntities('1.2.0')).toBe(true);
  expect(fixedMoneyEntities('2.0.0')).toBe(true);
  expect(fixedMoneyEntities('1.0.9')).toBe(false);
  expect(fixedMoneyEntities(undefined)).toBeNull();
  expect(fixedMoneyEntities('dev')).toBeNull();
});
