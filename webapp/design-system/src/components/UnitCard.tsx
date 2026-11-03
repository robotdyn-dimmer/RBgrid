import React from 'react';
import { EntityCard } from './EntityCard';

export interface UnitCardProps {
  /** Unit name (e.g. "Unit 9020"). */
  name: string;
  /** Active power in watts (null = unavailable). */
  powerW: number | null;
  /** RMS voltage in volts. */
  voltageV?: number | null;
  /** RMS current in amps. */
  currentA?: number | null;
  /** Power factor (0..1). */
  powerFactor?: number | null;
  /** Hardware present / live (false → offline, dimmed). */
  present?: boolean;
  /** Data source label (e.g. "I2C", "ADC"). */
  source?: string;
  /** Unit id (hex). */
  unitId?: string;
}

function fmtPower(w: number | null | undefined): string {
  if (w === null || w === undefined) return '—';
  return Math.abs(w) >= 1000 ? `${(w / 1000).toFixed(2)} kW` : `${Math.round(w)} W`;
}
function fmtNum(v: number | null | undefined, unit: string, digits = 1): string {
  return v === null || v === undefined ? '—' : `${v.toFixed(digits)} ${unit}`;
}

/**
 * RBGrid power-unit card — headline power + V/I/PF glance. Composes EntityCard.
 * @category Dashboard
 */
export const UnitCard: React.FC<UnitCardProps> = ({
  name,
  powerW,
  voltageV,
  currentA,
  powerFactor,
  present = true,
  source,
  unitId,
}) => (
  <EntityCard
    name={name}
    value={fmtPower(powerW)}
    present={present}
    metrics={[
      { label: 'Voltage', value: fmtNum(voltageV, 'V', 1) },
      { label: 'Current', value: fmtNum(currentA, 'A', 2) },
      { label: 'Power factor', value: powerFactor == null ? '—' : powerFactor.toFixed(2) },
    ]}
    caption={[source ? source.toUpperCase() : undefined, unitId].filter(Boolean).join(' · ') || undefined}
  />
);
