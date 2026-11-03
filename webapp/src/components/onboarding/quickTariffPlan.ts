/**
 * A simple tariff from what a bill says (wizard on a fresh hub 17.10, W191-07/08): one price per
 * kWh — as printed, usually incl. VAT — or a day + night pair, plus VAT %. The templates used to
 * open the full editor on the factory test plan (two zones, levies, net metering); a correct 0,35 €
 * incl. VAT took a calculator and ~14 steps. Built on the hub's current plan so its metadata stays.
 */
import { TariffPlan, TariffZone } from '../../types/tariff-plan';

export interface QuickTariffInput {
  currency: string;
  /** Price per kWh as entered. */
  price: number;
  /** Night price (two-zone) or null for one price all day. */
  nightPrice: number | null;
  /** Night hours [from, to) — default 23–7. */
  nightFrom?: number;
  nightTo?: number;
  /** The prices include VAT (as on a bill). */
  inclVat: boolean;
  vatPercent: number;
  /** What the grid pays per exported kWh; null = no export. */
  sellPrice?: number | null;
  name?: string;
}

const round4 = (v: number) => Math.round(v * 10000) / 10000;

/** Price without VAT, as the hub stores it. */
export const netOf = (price: number, inclVat: boolean, vat: number): number => round4(inclVat && vat > 0 ? price / (1 + vat / 100) : price);

const range = (fromH: number, toH: number) => ({
  condition_type: 'TIME_RANGE', is_active: true, priority: 1,
  params: { start_hour: fromH, start_minute: 0, end_hour: toH, end_minute: 0, spans_midnight: toH <= fromH },
});

export function quickPlan(base: Partial<TariffPlan> | null | undefined, q: QuickTariffInput): TariffPlan {
  const nf = q.nightFrom ?? 23, nt = q.nightTo ?? 7;
  const sell = q.sellPrice != null && q.sellPrice > 0 ? round4(q.sellPrice) : 0;
  const zone = (id: string, rate: number, cond: ReturnType<typeof range>): TariffZone => ({
    zone_id: id, zone_name: '', rate_buying_per_kwh: netOf(rate, q.inclVat, q.vatPercent), rate_selling_per_kwh: sell,
    is_enabled: true, conditions: [cond],
  } as unknown as TariffZone);
  // One price: one zone over the whole day (00–24), not the test plan's «Day 07–23» with a hole.
  const zones = q.nightPrice != null
    ? [zone('DAY', q.price, range(nt, nf)), zone('NIGHT', q.nightPrice, range(nf, nt))]
    : [zone('DAY', q.price, range(0, 0))];
  return {
    ...(base ?? {}),
    plan_name: q.name?.trim() || (base?.plan_name && !/test/i.test(base.plan_name) && !(base as { is_placeholder?: boolean }).is_placeholder ? base.plan_name : 'Tariff'),
    provider_name: base?.provider_name && !/test/i.test(base.provider_name) ? base.provider_name : '',
    description: '',
    plan_type: q.nightPrice != null ? 'TWO_ZONE' : 'SIMPLE',
    currency_code: q.currency,
    vat_percent: q.vatPercent,
    // The bill price already holds the levies: none on top (the test plan carried three).
    energy_tax_per_kwh: 0, grid_usage_fee_per_kwh: 0, renewable_levy_per_kwh: 0,
    net_metering_enabled: false,
    default_selling_rate_per_kwh: sell,
    fixed_payments: [], fixed_payments_count: 0,
    progressive_config: { ...((base?.progressive_config as object) ?? {}), is_enabled: false },
    zones,
    active_zones_count: zones.length,
    default_zone_id: 'DAY',
    is_active: true, is_default: true,
  } as unknown as TariffPlan;
}

/** All-in price per kWh the hub will compute from what is stored (for the «Total» line). */
export const grossOfInput = (price: number, inclVat: boolean, vat: number): number => (inclVat ? price : round4(price * (1 + vat / 100)));

/**
 * «One price all day» in the full editor (W191-19): the plan type is only a label on the hub, so
 * switching it to «Simple» left the test plan's «Day 07–23» zone, its night zone and its
 * description. Keeps the first enabled zone (its prices), over the whole day, nothing else.
 */
export function toSingleZone(plan: TariffPlan): Partial<TariffPlan> {
  const zones = (plan.zones ?? []) as TariffZone[];
  const keep = zones.find((z) => z.is_enabled) ?? zones[0];
  if (!keep) return {};
  const one = { ...keep, is_enabled: true, conditions: [range(0, 0)] } as unknown as TariffZone;
  const desc = (plan as { description?: string }).description;
  return {
    zones: [one],
    active_zones_count: 1,
    default_zone_id: keep.zone_id,
    plan_type: 'SIMPLE',
    ...(desc && /test/i.test(desc) ? { description: '' } : {}),
  } as unknown as Partial<TariffPlan>;
}

/**
 * The hub's plan carries more than the wizard sets (taxes / levies, net metering, progressive,
 * fixed payments, more than two zones) — saving the quick tariff would replace all that (docs review
 * 04.11), so the wizard asks first.
 */
export function richPlan(p: TariffPlan | undefined): boolean {
  if (!p) return false;
  const levy = (x?: number) => typeof x === 'number' && x > 0;
  return levy(p.energy_tax_per_kwh) || levy(p.grid_usage_fee_per_kwh) || levy(p.renewable_levy_per_kwh)
    || p.net_metering_enabled === true || p.progressive_config?.is_enabled === true
    || (p.fixed_payments?.length ?? 0) > 0 || (p.zones?.length ?? 0) > 2;
}
