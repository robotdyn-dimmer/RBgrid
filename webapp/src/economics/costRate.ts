/**
 * Money from live power — pure helpers (review 10 §1). Source of truth for the rate is the
 * DEVICE: telemetry `system.tariff{zone, buy_rate, sell_rate, cost_rate_h, currency}` (root E1,
 * canon fc0f962b). Per-line ₽/h = line P × buy_rate (root: no per-line money fields will come).
 * Everything derived here is an ESTIMATE of the moment → shown with «≈».
 */
import type { TelemetryTariff } from '../types/telemetry-units';
import type { TariffPlan } from '../types/tariff-plan';

export interface LiveRate {
  /** Buy rate per kWh, or null when no active plan (rates 0 on the wire). */
  buy: number | null;
  /** Sell (export) rate per kWh, or null. */
  sell: number | null;
  zone: string | null;
  currency: string | null;
  /** The device's own money/hour on NET power (negative = earning), or null. */
  netPerHour: number | null;
  /** The buy rate includes the plan's levies and VAT (the basis of the hub's `cost_day`). */
  gross?: boolean;
}

/** (rate + per-kWh levies) × (1 + VAT) — the basis the hub's day cost and hour records use
 *  (3φ audit L3P-02/H3P-02: «0.3000 €/kWh» and «€0.07/h» beside «Today €1.26» for 3.7 kWh =
 *  0.39 €/kWh). Identity when the plan has neither. */
export function grossOf(plan: Partial<TariffPlan> | null | undefined): ((rate: number) => number) | null {
  if (!plan) return null;
  const lev = (plan.energy_tax_per_kwh ?? 0) + (plan.grid_usage_fee_per_kwh ?? 0) + (plan.renewable_levy_per_kwh ?? 0);
  const vat = plan.vat_percent ?? 0;
  if (lev <= 0 && vat <= 0) return null;
  return (rate: number) => (rate + lev) * (1 + vat / 100);
}

export function liveRate(t: TelemetryTariff | undefined, plan?: Partial<TariffPlan> | null): LiveRate {
  const net = t?.buy_rate && t.buy_rate > 0 ? t.buy_rate : null;
  // The hub's own all-in rate wins (root 16.10: cost_rate_h is then on it too); else the plan's
  // levies and VAT on top of the zone rate.
  const allIn = net != null && t?.buy_rate_all_in && t.buy_rate_all_in > 0 ? t.buy_rate_all_in : null;
  const g = allIn != null ? null : grossOf(plan);
  // The live rate on the day cost's basis; the sell (feed-in) rate stays as the plan states it.
  const buy = allIn ?? (net != null && g ? g(net) : net);
  const sell = t?.sell_rate && t.sell_rate > 0 ? t.sell_rate : null;
  const perH = net != null && typeof t?.cost_rate_h === 'number' ? t.cost_rate_h : null;
  return {
    buy,
    sell,
    zone: buy != null ? t?.zone ?? null : null,
    currency: t?.currency ?? null,
    // The hub's €/h is net of levies and VAT: scale its import part to the same basis.
    netPerHour: perH != null && g && net != null && perH > 0 ? perH * ((buy as number) / net) : perH,
    gross: allIn != null || (!!g && net != null),
  };
}

/**
 * Money per hour for a signed power (W): consumption × buy rate (cost, positive);
 * export × sell rate (earning, negative). null when the rate needed is unknown.
 */
export function moneyPerHour(signedW: number | null | undefined, r: LiveRate): number | null {
  if (signedW == null) return null;
  const kw = signedW / 1000;
  if (kw >= 0) return r.buy != null ? kw * r.buy : null;
  return r.sell != null ? kw * r.sell : 0; // export without a sell rate earns nothing
}

/**
 * The value of generation NOW (UX audit GEN-05): what the site uses itself saves the BUY price,
 * only the surplus that leaves through the input earns the SELL price. `gridW` = the input's
 * signed power (− export). Returned with the earning sign (negative = money in), like
 * {@link moneyPerHour}. Valuing all of it at the sell rate understated solar ~3.6× in the demo.
 */
export function generationPerHour(genW: number | null | undefined, gridW: number | null | undefined, r: LiveRate): number | null {
  if (genW == null) return null;
  const g = Math.abs(genW) / 1000;
  if (gridW == null) return moneyPerHour(genW <= 0 ? genW : -genW, r);
  const exported = Math.min(g, Math.max(0, -gridW / 1000));
  const self = g - exported;
  if (r.buy == null && r.sell == null) return null;
  return -(self * (r.buy ?? 0) + exported * (r.sell ?? 0));
}

/** Money for an energy amount (Wh) at the current buy rate — an estimate («≈»). */
export function moneyForWh(wh: number | null | undefined, r: LiveRate): number | null {
  if (wh == null || r.buy == null) return null;
  return (wh / 1000) * r.buy;
}

/** Average price of TODAY's energy at the input: cost_day / e_day_in, rev_day / e_day_out. */
export interface DayRate { buy: number | null; sell: number | null }

/**
 * One rule for «сегодня» in money (rerun 16.10 R-T1: a point's day was priced at the CURRENT
 * zone — kitchen 4,82 ₽ on its page vs 1,92 ₽ on the Overview, lines summing above the input):
 * any day amount = its kWh × the input's average price of the day. The share-of-the-bill card
 * already worked this way; the sun's «не купили» uses the same price.
 */
export function dayAvgRate(t: { e_day_in?: number | null; cost_day?: number | null; e_day_out?: number | null; rev_day?: number | null } | null | undefined): DayRate {
  const per = (m?: number | null, wh?: number | null) => (m != null && m > 0 && wh != null && wh > 1 ? m / (wh / 1000) : null);
  return { buy: per(t?.cost_day, t?.e_day_in), sell: per(t?.rev_day, t?.e_day_out) };
}

export interface SolarValue {
  /** Produced energy the site used itself (Wh) and the part that left through the input. */
  selfWh: number;
  soldWh: number;
  /** ≈ money not spent on the grid: self × the average buying price of the period. */
  saved: number | null;
  /** Money for the export: the hub's revenue when it has one, else export × sell. */
  sold: number | null;
  /** saved + sold — «солнце дало ≈ X» (null when neither is known). */
  total: number | null;
}

/**
 * The sun's value — ONE formula for every screen (rerun 16.10 GEN-R01/HA-R07/HOME-R06: 3,34 /
 * 0,94 / 1,08 / 0,81 ₽ for the same day): self-use at the period's average buying price of the
 * input + the export's revenue. Economics uses it over its period, the Overview over today.
 */
export function solarValue(genWh: number, exportWh: number | null | undefined, avgBuy: number | null, revenue: number | null | undefined, sell?: number | null): SolarValue {
  const soldWh = Math.min(genWh, Math.max(0, exportWh ?? 0));
  const selfWh = Math.max(0, genWh - soldWh);
  const saved = avgBuy != null ? (selfWh / 1000) * avgBuy : null;
  const sold = revenue != null && revenue > 0 ? revenue : soldWh === 0 ? 0 : sell != null ? (soldWh / 1000) * sell : null;
  const total = saved == null && sold == null ? null : (saved ?? 0) + (sold ?? 0);
  return { selfWh, soldWh, saved, sold, total };
}

/** Today's generation (Σ RENEWABLE day output) and its value for the lens (solarDay). */
export function solarDayOf(genWh: number, t: { e_day_out?: number | null; rev_day?: number | null } | null | undefined, avgBuy: number | null, sell: number | null): { wh: number; money: number | null } {
  return { wh: genWh, money: genWh > 0 ? solarValue(genWh, t?.e_day_out, avgBuy, t?.rev_day, sell).total : null };
}
