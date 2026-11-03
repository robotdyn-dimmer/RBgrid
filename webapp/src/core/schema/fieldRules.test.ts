import { numberRules } from './fieldRules';
import { decimalError } from '../../components/common/DecimalField';

describe('settings number rules (W1-7)', () => {
  it('EV charge threshold cannot be negative or empty', () => {
    const r = numberRules('charge_threshold_w');
    expect(decimalError('-50', r)).not.toBeNull();
    expect(decimalError('', r)).not.toBeNull();
    expect(decimalError('1500', r)).toBeNull();
  });
  it('unknown duration keys are non-negative; other unknown keys just need a number', () => {
    expect(decimalError('-1', numberRules('foo_timeout_ms'))).not.toBeNull();
    expect(decimalError('-1', numberRules('offset'))).toBeNull();
    expect(decimalError('abc', numberRules('offset'))).not.toBeNull();
  });
  it('zero is «not set» where the hub says so; mirrors are read-only', () => {
    expect(decimalError('0', numberRules('nominal_voltage'))).toBeNull();
    expect(decimalError('50', numberRules('nominal_voltage'))).not.toBeNull();
    expect(numberRules('nominal_v').readOnly).toBe(true);
    expect(decimalError('1441', numberRules('idle_timeout_min'))).not.toBeNull();
  });
  it('a hub entry of another section does not apply', () => {
    const limits = { shortNameBytes: 24, nameBytes: 31, originsMax: 8, ranges: { foo_w: { min: 5, section: 'hardware' } } };
    expect(numberRules('foo_w', limits, 'system').min).toBe(0);
    expect(numberRules('foo_w', limits, 'hardware').min).toBe(5);
  });
  it('the hub ranges win', () => {
    const limits = { shortNameBytes: 24, nameBytes: 31, originsMax: 8, ranges: { charge_threshold_w: { min: 100, max: 7400 } } };
    expect(decimalError('50', numberRules('charge_threshold_w', limits))).not.toBeNull();
    expect(decimalError('9000', numberRules('charge_threshold_w', limits))).not.toBeNull();
  });
});
