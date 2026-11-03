/**
 * UnitCard — HA glance-style card for one unit. Thin adapter that maps the unit's
 * entity-group onto the @rbgrid/design-system <UnitCard> (single brand source).
 */
import React from 'react';
import { UnitCard as DSUnitCard } from '@rbgrid/design-system';
import { UnitEntityGroup, EntityState } from '../../entities/model';

/** Find a unit's entity by metric suffix (exact match). */
function entity(group: UnitEntityGroup, suffix: string): EntityState | undefined {
  return group.entities.find((e) => e.entity_id === `sensor.rbgrid_${group.unitKey}_${suffix}`);
}

/** Numeric reading for a metric suffix, or null when unavailable. */
function num(group: UnitEntityGroup, suffix: string): number | null {
  const e = entity(group, suffix);
  return typeof e?.state === 'number' ? e.state : null;
}

export const UnitCard: React.FC<{ group: UnitEntityGroup }> = ({ group }) => (
  <DSUnitCard
    name={group.name}
    powerW={num(group, 'power')}
    voltageV={num(group, 'voltage')}
    currentA={num(group, 'current')}
    powerFactor={num(group, 'power_factor')}
    present={group.present}
    source={group.src}
    unitId={group.unitKey}
  />
);
