import { parseDecimal, decimalError } from './DecimalField';

describe('DecimalField parsing (UX audit TAR-04/05)', () => {
  it('accepts the Russian comma and the dot alike', () => {
    expect(parseDecimal('6,43')).toBe(6.43);
    expect(parseDecimal('6.43')).toBe(6.43);
    expect(parseDecimal(' 1 200,5')).toBe(1200.5);
    expect(parseDecimal('')).toBeNull();
    expect(Number.isNaN(parseDecimal('abc') as number)).toBe(true);
    expect(Number.isNaN(parseDecimal('6,4,3') as number)).toBe(true);
  });

  it('checks range, decimals and required', () => {
    expect(decimalError('-5', { min: 0 })).toMatch(/0/);
    expect(decimalError('250', { min: 0, max: 100 })).toMatch(/100/);
    expect(decimalError('6,432199', { decimals: 4 })).toMatch(/4/);
    expect(decimalError('', {})).not.toBeNull();
    expect(decimalError('', { required: false })).toBeNull();
    expect(decimalError('6,43', { min: 0, max: 1000, decimals: 4 })).toBeNull();
  });
});

import { zoneDisplayName } from '../../economics/tariffPlan';

describe('zone names (UX audit HOME-11/TAR-12)', () => {
  it('factory English names read in the UI language; user names stay', () => {
    expect(zoneDisplayName('NIGHT', 'Night')).toBe('Ночь');
    expect(zoneDisplayName('DAY', 'Day')).toBe('День');
    expect(zoneDisplayName('DAY', 'Дневной льготный')).toBe('Дневной льготный');
    expect(zoneDisplayName('NIGHT', '')).toBe('Ночь');
  });
});
