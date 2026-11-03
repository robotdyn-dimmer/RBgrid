import { quickPlan, netOf, grossOfInput, toSingleZone } from './quickTariffPlan';

const base = { plan_name: 'Test Day/Night', provider_name: 'Test Provider', energy_tax_per_kwh: 0.02, net_metering_enabled: true, plan_version: 3 } as never;

describe('quick tariff (wizard)', () => {
  it('a bill price incl. 19 % VAT is stored net, the hub adds it back (0,35 → 0,2941)', () => {
    expect(netOf(0.35, true, 19)).toBe(0.2941);
    expect(grossOfInput(0.2941, false, 19)).toBeCloseTo(0.35, 3);
  });
  it('one price = one zone over the whole day; no levies, no net metering, test names dropped', () => {
    const p = quickPlan(base, { currency: 'EUR', price: 0.35, nightPrice: null, inclVat: true, vatPercent: 19 }) as unknown as Record<string, any>;
    expect(p.zones).toHaveLength(1);
    expect(p.zones[0].conditions[0].params).toMatchObject({ start_hour: 0, end_hour: 0, spans_midnight: true });
    expect(p.energy_tax_per_kwh).toBe(0);
    expect(p.net_metering_enabled).toBe(false);
    expect(p.plan_name).toBe('Tariff');
    expect(p.provider_name).toBe('');
    expect(p.plan_version).toBe(3);
  });
  it('day + night split at 23–7', () => {
    const p = quickPlan(base, { currency: 'EUR', price: 0.4, nightPrice: 0.25, inclVat: false, vatPercent: 0, name: 'Мой тариф' }) as unknown as Record<string, any>;
    expect(p.plan_type).toBe('TWO_ZONE');
    expect(p.zones.map((z: any) => [z.zone_id, z.rate_buying_per_kwh, z.conditions[0].params.start_hour, z.conditions[0].params.end_hour])).toEqual([['DAY', 0.4, 7, 23], ['NIGHT', 0.25, 23, 7]]);
    expect(p.plan_name).toBe('Мой тариф');
  });
});

describe('one zone all day (W191-19)', () => {
  it('keeps the first enabled zone over 00–24 and drops the test description', () => {
    const plan = {
      description: 'Simple two-zone test plan', plan_type: 'SIMPLE', default_zone_id: 'NIGHT',
      zones: [
        { zone_id: 'NIGHT', is_enabled: false, rate_buying_per_kwh: 0.1, conditions: [] },
        { zone_id: 'DAY', is_enabled: true, rate_buying_per_kwh: 0.3, rate_selling_per_kwh: 0.08, conditions: [{ params: { start_hour: 7, end_hour: 23 } }] },
        { zone_id: 'PEAK', is_enabled: true, rate_buying_per_kwh: 0.5, conditions: [] },
      ],
    } as never;
    const p = toSingleZone(plan) as Record<string, unknown>;
    const zones = p.zones as Array<Record<string, unknown>>;
    expect(zones).toHaveLength(1);
    expect(zones[0].zone_id).toBe('DAY');
    expect(zones[0].rate_selling_per_kwh).toBe(0.08);
    expect((zones[0].conditions as Array<{ params: Record<string, unknown> }>)[0].params).toMatchObject({ start_hour: 0, end_hour: 0, spans_midnight: true });
    expect(p.default_zone_id).toBe('DAY');
    expect(p.description).toBe('');
  });
});
