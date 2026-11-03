/**
 * Display formatters for the redesigned UI (spec §2.2 / §9 #7): figures in the ACTIVE locale
 * (i18n: ru → decimal comma, en → point), «—» for missing values, unit labels from the
 * `units` phrase file. (File name kept for history; it is no longer ru-only.)
 */
import { intlLocale, t, tCode } from '../../i18n';

export const nf = (v: number, d = 0): string =>
  // A value that rounds to zero prints «0», never «-0,0».
  (Math.abs(v) < 0.5 * 10 ** -d ? 0 : v).toLocaleString(intlLocale(), { minimumFractionDigits: d, maximumFractionDigits: d });

/** Power, input in WATTS → "1,85 кВт" / "640 Вт" / "—". */
export function pwW(w: number | null | undefined): string {
  if (w == null) return '—';
  const a = Math.abs(w);
  // Under 10 W one decimal — a standby load of 0,5 W or 4,7 W must not read «0 Вт» / «5 Вт» (Ф6:
  // the hub resolves ~0,1 W, home vampires live there).
  return a >= 1000 ? `${nf(w / 1000, 2)} ${t('units.kW')}` : a > 0 && a < 10 ? `${nf(w, 1)} ${t('units.W')}` : `${nf(w)} ${t('units.W')}`;
}

/** Power, input in KILOWATTS → "1,85 кВт" / "640 Вт" / «нет данных». */
export function pwKw(kw: number | null | undefined): string {
  if (kw == null) return t('common.noData');
  const a = Math.abs(kw);
  return a < 1 ? `${nf(a * 1000)} ${t('units.W')}` : `${nf(a, 2)} ${t('units.kW')}`;
}

/** Energy in WATT-HOURS → "3,2 кВт·ч" / "—". */
export function enWh(wh: number | null | undefined): string {
  if (wh == null) return '—';
  return `${nf(wh / 1000, 1)} ${t('units.kWh')}`;
}

export function curA(a: number | null | undefined): string {
  if (a == null) return '—';
  const x = Math.abs(a);
  // Small currents keep their digits: 0,064 A read «0,1 А» on the live 3φ input (.110 L2 lamp)
  // and Ф6 steps go down to 1 mA — under 0,1 A in mA (one decimal under 10 mA), under 1 A two decimals.
  if (x > 0 && x < 0.1) return `${nf(a * 1000, x < 0.01 ? 1 : 0)} ${t('units.mA')}`;
  return `${nf(a, x < 1 ? 2 : 1)} ${t('units.A')}`;
}

/** Reactive power with its character: Q > 0 inductive, Q < 0 capacitive (rbamp-dev contract);
 *  a Q too small against P (< 1 var or < 5 % of |P|) carries no word. */
export function reactive(q: number | null | undefined, p?: number | null): string {
  if (q == null) return '—';
  const word = Math.abs(q) >= Math.max(1, 0.05 * Math.abs(p ?? 0)) ? ` ${t(q > 0 ? 'units.inductive' : 'units.capacitive')}` : '';
  return `${nf(q, Math.abs(q) < 10 ? 1 : 0)} ${t('units.var')}${word}`;
}

/** The character word alone (for a cos φ figure): «инд.» / «ёмк.» / ''. */
export function qCharacter(q: number | null | undefined, p?: number | null): string {
  if (q == null || Math.abs(q) < Math.max(1, 0.05 * Math.abs(p ?? 0))) return '';
  return t(q > 0 ? 'units.inductive' : 'units.capacitive');
}

export function volt(v: number | null | undefined): string {
  return v == null ? '—' : `${nf(v)} ${t('units.V')}`;
}

export function freqHz(f: number | null | undefined): string {
  return f == null ? '—' : `${nf(f, 2)} ${t('units.Hz')}`;
}

/** Firmware tariff_zone enum (schema enums.tariff_zone) → phrase (`zone` file); unknown → as-is. */
export const zoneLabel = (z: string | null | undefined): string | null => (z ? tCode('zone', z) : null);

const CURRENCY_SIGN: Record<string, string> = { RUB: '₽', EUR: '€', USD: '$', GBP: '£', KZT: '₸', UAH: '₴', BYN: 'Br' };

/** Display sign for an ISO 4217 code (falls back to the code itself); '' when unknown. */
export const currencySign = (code: string | null | undefined): string =>
  code ? CURRENCY_SIGN[code] ?? code : '';

/**
 * Money in the device's currency. Currency + decimals come from the device (schema.money /
 * telemetry system.tariff.currency) — never hardcoded (operator rule; ISO minor units 0/2/3).
 */
export function money(v: number | null | undefined, currencyCode?: string | null, decimals = 2): string {
  if (v == null) return '—';
  // The reader's convention for where the sign goes (TAR-21: «0.65 $» in English) — «6,43 ₽»,
  // «$0.65», «0,46 €»; Intl's narrow spaces become plain ones.
  const iso = currencyCode && /^[A-Z]{3}$/.test(currencyCode) ? currencyCode : null;
  if (iso) {
    try {
      return v.toLocaleString(intlLocale(), { style: 'currency', currency: iso, currencyDisplay: 'narrowSymbol', minimumFractionDigits: decimals, maximumFractionDigits: decimals })
        .replace(/[\u00a0\u202f]/g, ' ');
    } catch { /* an engine without narrowSymbol — the plain form below */ }
  }
  const sign = currencySign(currencyCode);
  return sign ? `${nf(v, decimals)} ${sign}` : nf(v, decimals);
}

/** A per-kWh rate in the device's currency (rates carry up to 4 decimals). */
export function rate(v: number | null | undefined, currencyCode?: string | null, decimals = 4): string {
  if (v == null) return '—';
  // Same convention as money() — «€0.392/kWh», «0,392 €/кВт·ч» (L3P-25: «0.3920 €/kWh» beside
  // «€1.26»); up to `decimals`, trailing zeros dropped, never under 2.
  const iso = currencyCode && /^[A-Z]{3}$/.test(currencyCode) ? currencyCode : null;
  if (iso) {
    try {
      return v.toLocaleString(intlLocale(), { style: 'currency', currency: iso, currencyDisplay: 'narrowSymbol', minimumFractionDigits: Math.min(2, decimals), maximumFractionDigits: decimals })
        .replace(/[\u00a0\u202f]/g, ' ') + t('units.perKwh');
    } catch { /* an engine without narrowSymbol — the plain form below */ }
  }
  const sign = currencySign(currencyCode);
  return `${nf(v, decimals)}${sign ? ` ${sign}` : ''}${t('units.perKwh')}`;
}
