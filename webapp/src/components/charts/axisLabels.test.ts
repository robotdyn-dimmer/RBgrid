import { stepLabels } from './axisLabels';

const kwh = (wh: number) => `${(wh / 1000).toLocaleString('ru-RU', { minimumFractionDigits: 1, maximumFractionDigits: 1 })} кВт·ч`;
const volt = (v: number) => `${Math.round(v).toLocaleString('ru-RU')} В`;

describe('axis labels by step', () => {
  it('a fresh hub: 0–150 Wh in 50 Wh steps reads 0,00 · 0,05 · 0,10 · 0,15 kWh, not 0,1 · 0,1', () => {
    expect(stepLabels([0, 50, 100, 150], kwh, 'ru-RU')).toEqual(['0,00 кВт·ч', '0,05 кВт·ч', '0,10 кВт·ч', '0,15 кВт·ч']);
  });
  it('a 2.5 V step keeps the half volt', () => {
    expect(stepLabels([225, 227.5, 230, 232.5], volt, 'ru-RU')).toEqual(['225,0 В', '227,5 В', '230,0 В', '232,5 В']);
  });
  it('labels that already differ stay as they are', () => {
    expect(stepLabels([0, 10000, 20000], kwh, 'ru-RU')).toEqual(['0,0 кВт·ч', '10,0 кВт·ч', '20,0 кВт·ч']);
  });
});
