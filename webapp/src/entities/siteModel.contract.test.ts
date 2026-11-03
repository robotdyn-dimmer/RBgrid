/**
 * Contract tests against REAL hub payloads (sandbox .139, canon fc0f962b, GET-only snapshots in
 * src/test/fixtures/fw-fc0f962b). These are exactly the shapes the demo used to hide
 * (review 02 §2): legacy supply keys "main"/"v_bus", HEX4 keys, the firmware role enum,
 * the `dir` enum, schema money/units. If firmware changes them, these fail first.
 */
import schema from '../test/fixtures/fw-fc0f962b/schema.json';
import telemetry from '../test/fixtures/fw-fc0f962b/telemetry-latest.json';
import unitsConfig from '../test/fixtures/fw-fc0f962b/units-config.json';
import { normalizeTelemetryFrame, directionLabel } from './model';
import { hexKey, apiUnitId, isMainKey, signedPowerW, displayName } from './siteModel';
import { buildUnitsJoin } from '../hooks/useUnitsConfig';
import { buildFlowFrame } from '../components/flow/flowFrame';
import { deriveCapabilities, RawSchema } from '../core/schema/capabilities';
import { extractConfigPayload } from '../core/transport/DirectTransport';
import { lineUnits, mainUnit } from '../pages/overviewData';
import type { TelemetryFrame } from '../types/telemetry-units';
import type { JsonObject } from '../core/json';

const frame = telemetry as unknown as TelemetryFrame;
const data = normalizeTelemetryFrame(frame);
const unitsList = (extractConfigPayload(unitsConfig).units ?? []) as JsonObject[];
const join = buildUnitsJoin(unitsList);

describe('fw fc0f962b contract — schema', () => {
  const caps = deriveCapabilities(schema as unknown as RawSchema);
  it('exposes money formatting, the role enum and legacy supply keys', () => {
    expect(caps.money.currencyCode).toMatch(/^[A-Z]{3}$/);
    expect(caps.money.rateDecimals).toBe(4);
    expect(caps.roleEnum).toEqual(expect.arrayContaining(['RENEWABLE_SOURCE', 'EV_CHARGING', 'STORAGE_SYSTEM']));
    expect(caps.roleEnum).not.toContain('GENERATION');
    expect(caps.legacySupplyKeys).toBe(true);
  });
});

describe('fw fc0f962b contract — telemetry ↔ units-config join', () => {
  it('the main input is keyed "main" in telemetry and resolves to unit 0x0006', () => {
    const main = mainUnit(data.units, join);
    expect(main?.unitKey).toBe('main');
    expect(isMainKey('main')).toBe(true);
    expect(join.forGroup(main!)?.name).toBe('Main Supply Current');
    expect(join.input.ratedPowerW).toBe(11500);
    expect(join.input.maxCurrentA).toBe(50);
    expect(apiUnitId('main')).toBe('0006');
  });

  it('v_bus joins the VOLTAGE_BUS unit (0x0007) and is not a line', () => {
    const vbus = data.units.find((u) => u.unitKey === 'v_bus')!;
    expect(join.forGroup(vbus)?.type).toBe('VOLTAGE_BUS');
    expect(lineUnits(data.units, join).map((u) => u.unitKey)).not.toContain('v_bus');
    expect(apiUnitId('v_bus')).toBe('0007');
  });

  it('rbAmp lines join by point and by HEX4 key, with the firmware role', () => {
    const g = data.units.find((u) => u.unitKey === '9020')!;
    const cfg = join.forGroup(g);
    expect(cfg?.role).toBe('DEVICE_LOAD');
    // The hub's factory channel name is shown in words until the point is named (HOME-17).
    // ch0 of the I3 module reads «I0», as printed on the module (operator via root 16.10).
    expect(displayName(g, cfg)).toMatch(/30 (А|A) \(модуль I3, I0\)/);
    expect(join.roleByUnit['9020']).toBe('DEVICE_LOAD');
    expect(lineUnits(data.units, join).map((u) => u.unitKey).sort()).toEqual(['9010', '9011', '9020', '9021']);
  });

  it('HEX4 keys are UPPERCASE and string ids are hex, not decimal', () => {
    expect(hexKey(0x6a00)).toBe('6A00');
    expect(hexKey('9020')).toBe('9020');
    expect(hexKey('0x6a00')).toBe('6A00');
    expect(hexKey(6)).toBe('0006');
  });

  it('builds a flow frame from the real frame without a demo-shaped main', () => {
    const f = buildFlowFrame(data, { roleByUnit: join.roleByUnit, nameOf: (g) => displayName(g, join.forGroup(g)) });
    expect(f.solar).toBeNull(); // no RENEWABLE_SOURCE configured on .139
    expect(f.loads.map((l) => l.name).some((n) => /30 (А|A).*I3/.test(n))).toBe(true);
    expect(f.loads.map((l) => l.id)).not.toContain('v_bus');
  });
});

describe('direction enum (rbpower_current_direction_t)', () => {
  it('1 = consuming, 2 = supplying, 0/3/4 = idle; negative accepted as export', () => {
    expect(directionLabel(1)).toBe('import');
    expect(directionLabel(2)).toBe('export');
    expect(directionLabel(0)).toBe('idle');
    expect(directionLabel(4)).toBe('idle');
    expect(directionLabel(-1)).toBe('export');
  });

  it('signedPowerW takes the sign from dir, not from p_act', () => {
    const d = normalizeTelemetryFrame({
      ...frame,
      units: { '9021': { present: true, p_act: 480, dir: 2, src: 'i2c', src_class: 'rbamp' } },
    } as unknown as TelemetryFrame);
    expect(signedPowerW(d.units[0])).toBe(-480);
  });
});

describe('current installs: no native ADC — the input is an rbAmp unit with role MAIN_SUPPLY', () => {
  // Operator 2026-10-13: the built-in ADC is legacy (RBGRID_ENABLE_NATIVE_ADC=0); 0006/0007 exist
  // only on old benches. The input must be found by role and addressed by its own HEX4.
  const i2cOnly = buildUnitsJoin([
    { unit_id: 0x9001, name: 'Ввод', type: 'CURRENT_LOAD', role: 'MAIN_SUPPLY', rated_power_w: 15000, max_current_a: 32 },
    { unit_id: 0x9010, name: 'Кухня', type: 'CURRENT_LOAD', role: 'ZONE_LOAD', rated_power_w: 3680 },
  ] as unknown as JsonObject[]);
  it('takes the input limit and REST id from the MAIN_SUPPLY rbAmp unit', () => {
    expect(i2cOnly.input).toEqual({ ratedPowerW: 15000, maxCurrentA: 32, hex: '9001' });
  });
  it('resolves the input whether telemetry keys it by HEX4 or by the legacy "main"', () => {
    const byHex = { unitKey: '9001' } as never;
    const byLegacy = { unitKey: 'main' } as never;
    expect(i2cOnly.forGroup(byHex)?.hex).toBe('9001');
    expect(i2cOnly.forGroup(byLegacy)?.hex).toBe('9001');
  });
});

describe('current installs: grid sign comes from p_act (no DIRECTION cap, root f451b6a5)', () => {
  const join2 = buildUnitsJoin([
    { unit_id: 0x9001, name: 'Ввод', type: 'CURRENT_LOAD', role: 'MAIN_SUPPLY' },
    { unit_id: 0x9002, name: 'Солнце', type: 'CURRENT_LOAD', role: 'RENEWABLE_SOURCE' },
  ] as unknown as JsonObject[]);
  const frame2 = {
    ts: 1760000000, net_pwr: -1200, net_dir: 2, system: {},
    units: {
      '9001': { p_act: -1200, v: 231, i: 5.2, present: true, src: 'i2c', src_class: 'rbamp' },
      '9002': { p_act: 3000, v: 231, i: 13, present: true, src: 'i2c', src_class: 'rbamp' },
    },
  } as unknown as TelemetryFrame;
  it('exports (negative grid) when the rbAmp input reports negative p_act', () => {
    const d = normalizeTelemetryFrame(frame2);
    const f = buildFlowFrame(d, { roleByUnit: join2.roleByUnit });
    expect(f.grid).toBeCloseTo(-1.2);
  });
});
