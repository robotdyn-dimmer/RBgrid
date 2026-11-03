import React from 'react';
import { render, screen } from '@testing-library/react';
import { MemoryRouter } from 'react-router-dom';
import { AlarmsPanel } from './AlarmsPanel';
import { normalizeTelemetryFrame } from '../../entities/model';

const groups = normalizeTelemetryFrame({
  v: 1, ts: 0, freq: 50, net_pwr: 0, net_dir: 0,
  units: {
    '0102': { src: 'i2c', present: true, age_s: 0.3, i: 33, p_act: 7500, clip: true, load_pct: 112 },
    '0103': { src: 'i2c', present: true, age_s: 0.3, i: 27, p_act: 6100, ct_warn: true, load_pct: 93 },
    '0101': { src: 'i2c', present: true, age_s: 0.3, i: 27, p_act: 6100, load_pct: 27 },
  },
} as never).units;

describe('AlarmsPanel — CT at its limit (BASIC clip / ct_warn)', () => {
  it('lists every clipping and near-limit channel with its load, the quiet one not', () => {
    render(<MemoryRouter><AlarmsPanel groups={groups} /></MemoryRouter>);
    const links = screen.getAllByRole('link').map((a) => a.getAttribute('href'));
    expect(links).toEqual(['/point/0102', '/point/0103']);
    expect(document.body.textContent).toMatch(/112/);
    expect(document.body.textContent).toMatch(/93/);
  });
  it('a point on a channel the module lacks is not «нет связи» (UI3 in place of UI7)', () => {
    const g = normalizeTelemetryFrame({ v: 1, ts: 0, freq: 50, net_pwr: 0, net_dir: 0,
      units: { '0101': { src: 'i2c', present: true, age_s: 0.3, i: 2, p_act: 430 }, '0105': { src: 'i2c', present: false, p_act: 0 } } } as never).units
      .map((x) => (x.unitKey === '0105' ? { ...x, noChannel: true } : x));
    render(<MemoryRouter><AlarmsPanel groups={g} /></MemoryRouter>);
    expect(screen.queryAllByRole('link')).toEqual([]);
  });
});
