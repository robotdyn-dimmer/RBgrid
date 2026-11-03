import { demoSensors, demoSensorModules, demoTelemetryFrame, demoTariffConfig, demoHistory } from './demo';
import { demoVariant } from './demoVariant';
import { dearZoneCharge } from '../economics/insights';
import { zoneTimeline } from '../economics/tariffPlan';
import { mockConfig } from '../core/mocks';
import { channelMissing } from '../types/sensors';

const variants = ['heatpump-ev', 'home', 'home-nosolar', 'business', 'ev'] as const;

describe('demo sites: each variant is a whole installation (operator 20.10)', () => {
  afterEach(() => localStorage.clear());
  it.each(variants)('%s — every point sits on a real channel of its module, telemetry and config agree', (v) => {
    localStorage.setItem('rbgrid.demo.variant', v);
    const mods = demoSensorModules();
    const sensors = demoSensors();
    for (const s of sensors) expect([v, s.ch, channelMissing(s, mods)]).toEqual([v, s.ch, false]);
    const units = Object.keys(demoTelemetryFrame().units).sort();
    const cfg = (mockConfig('units').units as { unit_id: number }[]).map((u) => u.unit_id.toString(16).toUpperCase().padStart(4, '0')).sort();
    expect(units).toEqual(cfg);
    // A v1.1.x hub keeps history for 8 points (root 02.11): no demo site shows more.
    expect(units.length).toBeLessThanOrEqual(8);
    expect(sensors.filter((s) => s.unit_id).map((s) => s.unit_id).sort()).toEqual(cfg);
  });
  it('variants differ in hardware and scenario', () => {
    const models = (v: string) => { localStorage.setItem('rbgrid.demo.variant', v); return demoSensorModules().map((m) => m.model).join(','); };
    expect(new Set(variants.map(models)).size).toBe(variants.length);
    localStorage.setItem('rbgrid.demo.variant', 'business');
    expect(mockConfig('system').site_profile).toBe('industrial');
    expect(demoTelemetryFrame().units['9001'].ph).toHaveLength(3);
  });
  it('heat pump + EV (site brief 02.11): the default, three measured legs, DE plan, the evening charge in the Day zone', () => {
    expect(demoVariant().id).toBe('heatpump-ev');
    window.history.replaceState(null, '', '/?scenario=flat');
    expect(demoVariant().id).toBe('home-nosolar');
    window.history.replaceState(null, '', '/');
    const f = demoTelemetryFrame();
    const legs = f.units['9001'].ph ?? [];
    expect(legs).toHaveLength(3);
    // Phase is a label: the legs sum the points on them (+ the rest), no fixed split.
    const sumLegs = legs.reduce((s, l) => s + (l.p ?? 0), 0);
    expect(Math.abs(sumLegs - (f.units['9001'].p_act ?? 0))).toBeLessThan(0.1 * Math.abs(f.units['9001'].p_act ?? 1) + 50);
    const plan = (demoTariffConfig().data as { list: Parameters<typeof zoneTimeline>[0][] }).list[0];
    expect([plan?.plan_name, plan?.provider_name, plan?.vat_percent, plan?.net_metering_enabled]).toEqual(['Two-zone, DE', 'Stadtwerke Demo', 19, false]);
    // Yesterday's charge by the hour, three phases summed, priced gross: ≈ 27.6 kWh × (0.38 − 0.29).
    const by = new Map<number, number>();
    for (const id of ['9030', '9031', '9032']) {
      for (const r of demoHistory('energy', 'hour', 60, id) as { ts: number; ec: number }[]) {
        const start = new Date((r.ts - 3600) * 1000);
        const y = new Date(); y.setHours(0, 0, 0, 0); y.setDate(y.getDate() - 1);
        if (start >= y && start.getTime() < y.getTime() + 86_400_000) by.set(start.getHours(), (by.get(start.getHours()) ?? 0) + r.ec);
      }
    }
    const spans = zoneTimeline(plan, 0).map((x) => ({ ...x, rate: x.rate * 1.19 }));
    const d = dearZoneCharge(by, spans);
    expect(d && Math.round(d.dearWh / 100) / 10).toBeCloseTo(27.6, 0);
    expect(d && Math.round(d.extra * 100) / 100).toBeCloseTo(2.48, 1);
  });
});
