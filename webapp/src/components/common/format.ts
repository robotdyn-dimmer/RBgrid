/**
 * Small display formatters for entity values.
 */

/** Format an entity state value with an optional unit. Returns "—" for null/undefined. */
export function fmt(
  value: number | string | null | undefined,
  unit?: string | null,
  digits = 1
): string {
  if (value === null || value === undefined) return '—';
  if (typeof value === 'string') return value;
  const abs = Math.abs(value);
  const text = abs >= 1000 ? Math.round(value).toLocaleString() : value.toFixed(digits);
  return unit ? `${text} ${unit}` : text;
}

/** Compact power with auto kW scaling. */
export function fmtPower(watts: number | null | undefined): string {
  if (watts === null || watts === undefined) return '—';
  return Math.abs(watts) >= 1000 ? `${(watts / 1000).toFixed(2)} kW` : `${Math.round(watts)} W`;
}

/** Compact energy (Wh) with auto kWh scaling. */
export function fmtEnergy(wh: number | null | undefined): string {
  if (wh === null || wh === undefined) return '—';
  return Math.abs(wh) >= 1000 ? `${(wh / 1000).toFixed(2)} kWh` : `${Math.round(wh)} Wh`;
}
