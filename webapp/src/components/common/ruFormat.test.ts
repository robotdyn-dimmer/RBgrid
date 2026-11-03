import { curA, reactive, qCharacter } from './ruFormat';

describe('small currents and reactive power (live 3φ on .110, Ф6)', () => {
  it('currents under 1 A keep their digits', () => {
    expect(curA(1.667)).toBe('1,7 А');
    expect(curA(0.553)).toBe('0,55 А');
    expect(curA(0.064)).toBe('64 мА');
    expect(curA(0.0012)).toBe('1,2 мА');
    expect(curA(0)).toBe('0,00 А');
  });
  it('Q carries inductive / capacitive, a tiny Q carries no word', () => {
    expect(reactive(-59.26, 326)).toBe('-59 вар ёмк.');
    expect(reactive(2000, 1600)).toBe('2\u00a0000 вар инд.');
    expect(reactive(-0.42, 14)).toBe('-0,4 вар');
    expect(qCharacter(-14.46, 114)).toBe('ёмк.');
    expect(qCharacter(-0.42, 14)).toBe('');
  });
});

describe('rate in the money convention (L3P-25)', () => {
  it('«0,392 €/кВт·ч» — up to 4 decimals, trailing zeros dropped', () => {
    // eslint-disable-next-line @typescript-eslint/no-var-requires
    const { rate } = require('./ruFormat');
    expect(rate(0.39204, 'EUR')).toBe('0,392 €/кВт·ч');
    expect(rate(0.3, 'EUR')).toBe('0,30 €/кВт·ч');
  });
});
