/**
 * Demo sites (operator 20.10: «несколько вариантов со своим оборудованием и сценариями — так
 * наглядно, а не просто набор мок-данных»). Each variant is a whole installation: its rbAmp
 * modules, which channel measures which point, the free channels, and a situation worth showing.
 * The telemetry, the sensor list, the units config and the history are all built from this one
 * table (mock/demo.ts, core/mocks.ts).
 *
 *  heatpump-ev   a three-phase house (STD-U3I3 input group, per-phase readings), one BASIC-UI3 per
 *                phase with that phase's EV-charger and heat-pump legs (+ the kitchen on L1) —
 *                8 points; laundry and lights unmetered. Two-zone DE
 *                tariff; the car charges on arrival (18:00–20:30, the Day zone), once a week at night.
 *  home          STD-UI1 on the input and one on the inverter line (export at noon), BASIC-UI3 with the
 *                car charger, the garage and the kitchen, a BASIC-UI1 on the water heater that
 *                stopped answering — «module not responding» (the site's Events shot, content 02.11).
 *  home-nosolar  a flat: the input on an STD-UI1, five circuits on a BASIC-UI5.
 *  business      a workshop: three-phase STD-U3I3 input (per-phase readings), one BASIC-UI3 per phase —
 *                the compressor runs above its line rating in working hours.
 *  ev            a car park: STD-UI1 on the input, BASIC-UI3 for building + lighting, a BASIC-UI3 with three
 *                charging stations — the stations' day: charging / waiting / free.
 *  us            (preview) a 120/240 V split-phase home on an STD-U3I3 group.
 */
import type { SensorModule } from '../types/sensors';
import { demoVariant, DemoVariantId } from './demoVariant';

/** A point's day profile (mock/demo.ts demoHourW). */
export type DemoRole =
  | 'input' | 'kitchen' | 'solar' | 'garage' | 'ev1' | 'ev2' | 'ev3' | 'off' | 'line'
  | 'workshop' | 'lighting' | 'compressor' | 'hvac' | 'office'
  | 'heater' | 'laundry' | 'living' | 'bedrooms' | 'building' | 'parkLight' | 'homeEv'
  | 'hpEv' | 'heatPump' | 'hpKitchen';

export interface DemoPoint {
  /** HEX4 unit id. */
  id: string;
  point: string;
  name: string;
  short: string;
  /** Firmware role. */
  role: string;
  profile: DemoRole;
  module: number;
  /** Channel; 254 = the module's phase group. */
  ch: number;
  /** Phase code (0 unset · 1–3 L1–L3 · 4 3P · 5 L1L2). */
  phase: number;
  ratedW: number;
  maxA: number;
  /** The CT on the channel: schema ct_model code and its rating (A) — load_pct / ct_warn / clip. */
  ct: { code: number; a: number };
  group?: number;
  /** The module doesn't answer. */
  absent?: boolean;
  /** One CT on a two-pole line reads one leg (US 240 V). */
  pole?: number;
}

export interface DemoSite {
  modules: SensorModule[];
  points: DemoPoint[];
  /** Channels with a sensor and no point. */
  free: { module: number; ch: number }[];
  /** Groups (system.groups). */
  groups: { id: number; name: string }[];
  /** The site's name. */
  title: string;
  /** Unmetered rest of the site, W (lights, sockets on no channel). */
  restW: (h: number, workday: boolean) => number;
  /** The site's own two-zone plan (default: the demo's «Two-rate (demo)»). */
  tariff?: DemoTariff;
  /** A three-phase input's legs from the points on each phase (+ the rest split evenly) rather
   *  than a fixed split — the phase that carries the kitchen reads higher. */
  legsFromPoints?: boolean;
}

/** A two-zone plan: Day [dayFrom, dayTo), Night the rest. Rates gross per kWh (incl. VAT). */
export interface DemoTariff {
  plan: string;
  provider: string;
  dayFrom: number;
  dayTo: number;
  dayGross: number;
  nightGross: number;
  vat: number;
  /** Fixed fee per month. */
  fixed: number;
  /** Export price per kWh; null = no net metering. */
  sell: number | null;
}

const CT5 = { code: 1, a: 5 };
const CT10 = { code: 2, a: 10 };
const CT30 = { code: 3, a: 30 };
const CT50 = { code: 4, a: 50 };
const CT100 = { code: 5, a: 100 };

/** Schema `enums.ct_model` of the demo hub (names as the hub gives them, d03f19ed). */
export const DEMO_CT_MODELS: Record<string, string> = {
  1: 'SCT-013-005, 5 A', 2: 'SCT-013-010, 10 A', 3: 'SCT-013-030, 30 A', 4: 'SCT-013-050, 50 A', 5: 'SCT-013-100, 100 A',
};

// BASIC-UI* meter consumption (BASIC-I* only watch current — not in the balance, operator 18.10;
// no BASIC sees export), so the solar and an exporting input sit on STD modules.
const basic = (module: number, model: string, nI: number, present = true): SensorModule =>
  ({ module, addr: 0x50 + module, present, line: 'basic', model, n_v: model.includes('U') ? 1 : 0, n_i: nI, cal: present ? 'n/a' : 'unknown' });
// STD modules ship with factory-paired, per-channel calibrated sensors (CT-): not replaceable, no model
// to pick (operator / root 04.11).
const std = (module: number, cal = 'ok'): SensorModule =>
  ({ module, addr: 0x50 + module, present: true, line: 'std', model: 'STD-U3I3', variant: 99, n_v: 3, n_i: 3, map: 54, period: 'counters', cal, ct_settable: false });
// A single-phase STD (operator 04.11: the grid input and an inverter line always on STD, for accurate metering).
const std1 = (module: number): SensorModule =>
  ({ module, addr: 0x50 + module, present: true, line: 'std', model: 'STD-UI1', n_v: 1, n_i: 1, period: 'counters', cal: 'ok', ct_settable: false });

const SITES: Record<DemoVariantId, () => DemoSite> = {
  // site brief 02.11 «Дом с тепловым насосом и EV»: the typical European kit — three-phase
  // 3×35 A, two-zone tariff, no solar. Phase is a label: each CT reads its own conductor, the
  // charger's and the heat pump's three phases are three points each (grouped), never ×3.
  'heatpump-ev': () => ({
    title: 'Demo house',
    // A BASIC module measures every channel against its ONE voltage input, so each phase gets its own
    // BASIC-UI3 with the voltage taken on that phase (root 04.11 — mixed phases on one BASIC read wrong).
    modules: [std(0), basic(1, 'BASIC-UI3', 3), basic(2, 'BASIC-UI3', 3), basic(3, 'BASIC-UI3', 3)],
    points: [
      { id: '9001', point: '', name: 'Main input 3φ', short: 'Main', role: 'MAIN_SUPPLY', profile: 'input', module: 0, ch: 254, phase: 4, ratedW: 24150, maxA: 35, ct: CT100 },
      // An 11 kW wallbox (3 × 16 A) on a 3 × 20 A line: charging reads ~80 % of the line, not «at the rating».
      { id: '9030', point: 'garage.ev.l1', name: 'EV charger L1', short: 'EV L1', role: 'EV_CHARGING', profile: 'hpEv', module: 1, ch: 0, phase: 1, ratedW: 4600, maxA: 20, ct: CT30, group: 1 },
      { id: '9031', point: 'garage.ev.l2', name: 'EV charger L2', short: 'EV L2', role: 'EV_CHARGING', profile: 'hpEv', module: 2, ch: 0, phase: 2, ratedW: 4600, maxA: 20, ct: CT30, group: 1 },
      { id: '9032', point: 'garage.ev.l3', name: 'EV charger L3', short: 'EV L3', role: 'EV_CHARGING', profile: 'hpEv', module: 3, ch: 0, phase: 3, ratedW: 4600, maxA: 20, ct: CT30, group: 1 },
      { id: '9040', point: 'heatpump.l1', name: 'Heat pump L1', short: 'HP L1', role: 'HEATING_COOLING', profile: 'heatPump', module: 1, ch: 1, phase: 1, ratedW: 1400, maxA: 10, ct: CT30, group: 2 },
      { id: '9041', point: 'heatpump.l2', name: 'Heat pump L2', short: 'HP L2', role: 'HEATING_COOLING', profile: 'heatPump', module: 2, ch: 1, phase: 2, ratedW: 1400, maxA: 10, ct: CT30, group: 2 },
      { id: '9042', point: 'heatpump.l3', name: 'Heat pump L3', short: 'HP L3', role: 'HEATING_COOLING', profile: 'heatPump', module: 3, ch: 1, phase: 3, ratedW: 1400, maxA: 10, ct: CT30, group: 2 },
      { id: '9020', point: 'kitchen', name: 'Kitchen', short: 'Kitchen', role: 'ZONE_LOAD', profile: 'hpKitchen', module: 1, ch: 2, phase: 1, ratedW: 3680, maxA: 16, ct: CT30 },
    ],
    free: [{ module: 2, ch: 2 }, { module: 3, ch: 2 }],
    groups: [{ id: 1, name: 'EV charger' }, { id: 2, name: 'Heat pump' }],
    // Unmetered: the laundry (10:00–11:30), lights & sockets and the fridge/router — ~12 kWh a day.
    restW: (h) => 85 + (h === 10 ? 2000 : h === 11 ? 1000 : 0)
      + (h < 6 ? 180 : h < 9 ? 400 : h < 17 ? 200 : h < 22 ? 500 : 300),
    // «Two-zone, DE» — a fictional provider, not a real brand.
    tariff: { plan: 'Two-zone, DE', provider: 'Stadtwerke Demo', dayFrom: 6, dayTo: 22, dayGross: 0.38, nightGross: 0.29, vat: 19, fixed: 12, sell: null },
    legsFromPoints: true,
  }),
  home: () => ({
    title: 'Demo Home',
    // The input and the inverter line each on an STD-UI1 (they see export; operator 04.11: always STD
    // there), a BASIC-UI3 for the garage, the charger and the kitchen, a BASIC-UI1 on the water heater
    // that stopped answering.
    modules: [std1(0), basic(1, 'BASIC-UI1', 1, false), basic(2, 'BASIC-UI3', 3), std1(3)],
    points: [
      { id: '9001', point: '', name: 'Main input', short: 'Main', role: 'MAIN_SUPPLY', profile: 'input', module: 0, ch: 0, phase: 1, ratedW: 13800, maxA: 63, ct: CT100 },
      { id: '9021', point: 'solar', name: 'Solar panels', short: 'Solar', role: 'RENEWABLE_SOURCE', profile: 'solar', module: 3, ch: 0, phase: 1, ratedW: 5000, maxA: 25, ct: CT30 },
      { id: '9020', point: 'floor1.kitchen', name: 'Kitchen', short: 'Kitchen', role: 'ZONE_LOAD', profile: 'kitchen', module: 2, ch: 2, phase: 1, ratedW: 3680, maxA: 16, ct: CT30, group: 1 },
      { id: '9010', point: 'floor1.boiler', name: 'Water heater', short: 'Water htr', role: 'HEATING_COOLING', profile: 'off', module: 1, ch: 0, phase: 1, ratedW: 2300, maxA: 10, ct: CT30, group: 1, absent: true },
      { id: '9030', point: 'garage.charger', name: 'Car charger', short: 'Charger', role: 'EV_CHARGING', profile: 'homeEv', module: 2, ch: 0, phase: 1, ratedW: 7400, maxA: 32, ct: CT50 },
      { id: '9031', point: 'garage.workshop', name: 'Garage', short: 'Garage', role: 'ZONE_LOAD', profile: 'garage', module: 2, ch: 1, phase: 1, ratedW: 3680, maxA: 16, ct: CT30 },
    ],
    free: [],
    groups: [{ id: 1, name: 'First floor' }],
    // Fridge, router, standby (~230 W) and daytime use: dishwasher, washing, home office.
    restW: (h) => 230 + (h >= 7 && h < 23 ? 260 : 0) + (h >= 10 && h < 15 ? 350 : 0),
  }),
  'home-nosolar': () => ({
    title: 'Demo flat',
    // The input on an STD-UI1 (operator 04.11: the grid input always on STD), the five circuits on a
    // BASIC-UI5.
    modules: [std1(0), basic(1, 'BASIC-UI5', 5)],
    points: [
      { id: '9001', point: '', name: 'Main input', short: 'Main', role: 'MAIN_SUPPLY', profile: 'input', module: 0, ch: 0, phase: 1, ratedW: 9200, maxA: 40, ct: CT50 },
      { id: '9020', point: 'kitchen', name: 'Kitchen', short: 'Kitchen', role: 'ZONE_LOAD', profile: 'kitchen', module: 1, ch: 0, phase: 1, ratedW: 3680, maxA: 16, ct: CT30 },
      // A 10 A CT on a 2 kW heater: ~87 % of the nominal while it heats.
      { id: '9010', point: 'bath.heater', name: 'Water heater', short: 'Water htr', role: 'HEATING_COOLING', profile: 'heater', module: 1, ch: 1, phase: 1, ratedW: 2300, maxA: 10, ct: CT10 },
      { id: '9021', point: 'bath.laundry', name: 'Laundry', short: 'Laundry', role: 'DEVICE_LOAD', profile: 'laundry', module: 1, ch: 2, phase: 1, ratedW: 2300, maxA: 10, ct: CT30 },
      { id: '9030', point: 'living', name: 'Living room', short: 'Living', role: 'ZONE_LOAD', profile: 'living', module: 1, ch: 3, phase: 1, ratedW: 2300, maxA: 10, ct: CT30, group: 1 },
      { id: '9031', point: 'bedrooms', name: 'Bedrooms', short: 'Bedrooms', role: 'ZONE_LOAD', profile: 'bedrooms', module: 1, ch: 4, phase: 1, ratedW: 2300, maxA: 10, ct: CT5, group: 1 },
    ],
    free: [],
    groups: [{ id: 1, name: 'Rooms' }],
    restW: (h) => 60 + (h >= 7 && h < 23 ? 40 : 0),
  }),
  business: () => ({
    title: 'Workshop (demo)',
    // One BASIC-UI3 per phase, its voltage input on that phase (a BASIC measures all its channels
    // against its one voltage input — root 04.11).
    modules: [std(0), basic(1, 'BASIC-UI1', 1), basic(2, 'BASIC-UI3', 3), basic(3, 'BASIC-UI3', 3)],
    points: [
      { id: '9001', point: '', name: 'Main input 3φ', short: 'Main', role: 'MAIN_SUPPLY', profile: 'input', module: 0, ch: 254, phase: 4, ratedW: 43000, maxA: 63, ct: CT100 },
      { id: '9020', point: 'shop.machines', name: 'Machine tools', short: 'Machines', role: 'DEVICE_LOAD', profile: 'workshop', module: 1, ch: 0, phase: 1, ratedW: 9000, maxA: 40, ct: CT50, group: 1 },
      // Rated 3 kW, cycling up to 3.8 kW in working hours — «above the line rating».
      { id: '9010', point: 'shop.compressor', name: 'Compressor', short: 'Compressor', role: 'DEVICE_LOAD', profile: 'compressor', module: 2, ch: 0, phase: 2, ratedW: 3000, maxA: 16, ct: CT30, group: 1 },
      { id: '9031', point: 'building.office', name: 'Office', short: 'Office', role: 'ZONE_LOAD', profile: 'office', module: 2, ch: 1, phase: 2, ratedW: 3000, maxA: 16, ct: CT30, group: 2 },
      { id: '9021', point: 'building.lighting', name: 'Lighting', short: 'Lighting', role: 'ZONE_LOAD', profile: 'lighting', module: 3, ch: 0, phase: 3, ratedW: 2000, maxA: 10, ct: CT30, group: 2 },
      { id: '9030', point: 'building.hvac', name: 'Ventilation & AC', short: 'HVAC', role: 'HEATING_COOLING', profile: 'hvac', module: 3, ch: 1, phase: 3, ratedW: 6000, maxA: 25, ct: CT30, group: 2 },
    ],
    free: [{ module: 2, ch: 2 }, { module: 3, ch: 2 }],
    groups: [{ id: 1, name: 'Production' }, { id: 2, name: 'Building' }],
    restW: (_h, workday) => 400 + (workday ? 200 : 0),
  }),
  ev: () => ({
    title: 'Car park (demo)',
    // The input on an STD-UI1 (operator 04.11: always STD), a BASIC-UI3 for the building and the
    // lighting, a BASIC-UI3 for the three stations.
    modules: [std1(0), basic(1, 'BASIC-UI3', 3), basic(2, 'BASIC-UI3', 3)],
    points: [
      { id: '9001', point: '', name: 'Main input', short: 'Main', role: 'MAIN_SUPPLY', profile: 'input', module: 0, ch: 0, phase: 1, ratedW: 22000, maxA: 100, ct: CT100 },
      { id: '9020', point: 'building', name: 'Building', short: 'Building', role: 'ZONE_LOAD', profile: 'building', module: 1, ch: 0, phase: 1, ratedW: 3680, maxA: 16, ct: CT30 },
      { id: '9021', point: 'parking.lighting', name: 'Car park lighting', short: 'Lighting', role: 'ZONE_LOAD', profile: 'parkLight', module: 1, ch: 1, phase: 1, ratedW: 1500, maxA: 10, ct: CT30 },
      { id: '9030', point: 'ev.station1', name: 'Charger 1', short: 'EV-1', role: 'EV_CHARGING', profile: 'ev1', module: 2, ch: 0, phase: 1, ratedW: 7400, maxA: 32, ct: CT50, group: 1 },
      { id: '9031', point: 'ev.station2', name: 'Charger 2', short: 'EV-2', role: 'EV_CHARGING', profile: 'ev2', module: 2, ch: 1, phase: 1, ratedW: 7400, maxA: 32, ct: CT50, group: 1 },
      { id: '9032', point: 'ev.station3', name: 'Charger 3', short: 'EV-3', role: 'EV_CHARGING', profile: 'ev3', module: 2, ch: 2, phase: 1, ratedW: 7400, maxA: 32, ct: CT50, group: 1 },
    ],
    free: [{ module: 1, ch: 2 }],
    groups: [{ id: 1, name: 'Charging stations' }],
    restW: () => 120,
  }),
  us: () => ({
    title: 'Demo US home',
    modules: [std(0, 'not_asserted'), basic(1, 'BASIC-UI1', 1, false), std(2), basic(3, 'BASIC-UI3', 3)],
    points: [
      { id: '9001', point: '', name: 'Main panel', short: 'Main', role: 'MAIN_SUPPLY', profile: 'input', module: 0, ch: 254, phase: 5, ratedW: 48000, maxA: 200, ct: CT100 },
      { id: '9020', point: 'floor1.kitchen', name: 'Kitchen', short: 'Kitchen', role: 'ZONE_LOAD', profile: 'kitchen', module: 2, ch: 1, phase: 1, ratedW: 1800, maxA: 15, ct: CT30, group: 1 },
      { id: '9021', point: 'solar', name: 'Solar PV', short: 'Solar', role: 'RENEWABLE_SOURCE', profile: 'solar', module: 2, ch: 0, phase: 5, ratedW: 7600, maxA: 40, ct: CT50, pole: 0.5 },
      { id: '9010', point: 'floor1.boiler', name: 'Water heater', short: 'Water htr', role: 'HEATING_COOLING', profile: 'off', module: 1, ch: 0, phase: 5, ratedW: 4500, maxA: 25, ct: CT30, group: 1, absent: true },
      { id: '9030', point: 'ev.station1', name: 'EV charger 1', short: 'EV-1', role: 'EV_CHARGING', profile: 'ev1', module: 3, ch: 0, phase: 5, ratedW: 9600, maxA: 40, ct: CT50, group: 2, pole: 0.5 },
      { id: '9031', point: 'ev.station2', name: 'EV charger 2', short: 'EV-2', role: 'EV_CHARGING', profile: 'ev2', module: 3, ch: 1, phase: 2, ratedW: 1440, maxA: 12, ct: CT30, group: 2 },
    ],
    free: [{ module: 2, ch: 2 }, { module: 3, ch: 2 }],
    groups: [{ id: 1, name: 'First floor' }, { id: 2, name: 'Garage' }],
    restW: (h) => 150 + (h >= 6 && h < 23 ? 120 : 0),
  }),
};

/** The active variant's site. */
export function demoSite(): DemoSite {
  return SITES[demoVariant().id]();
}

/** A demo point by unit id (upper-case HEX4); the legacy input key 0006 is the input. */
export function demoPoint(unitId?: string | null): DemoPoint | undefined {
  const id = (unitId ?? '9001').toUpperCase();
  return demoSite().points.find((p) => p.id === (id === '0006' ? '9001' : id));
}
