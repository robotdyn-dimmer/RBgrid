/**
 * Data-quality + time-quality decoding — contract v4 §1 (CONTRACT_V4_RC.md).
 *
 * Every v4 history record carries a `q` u16 bitmask and the telemetry frame carries `tq`.
 * The UI renders these per spec §5.3 / principle U3 ("truth of data"): q=0 is clean, some
 * bits make a value *uncertain* (hatch + note), a couple make it *bad* (greyed / excluded).
 *
 * The bit→name legend comes from `schema.quality_bits` (normalized), but we keep a canonical
 * fallback so the module works before the schema loads and if the device omits it.
 */

export type QualityLevel = 'good' | 'uncertain' | 'bad';

/** Canonical quality-bit names (contract §1). Index = bit position. */
export const QUALITY_BIT_NAMES: Record<number, string> = {
  0: 'PARTIAL',
  1: 'TIME_UNSYNC',
  2: 'CLOCK_STEP',
  3: 'SRC_GAP',
  4: 'RECOMPUTED',
  5: 'ESTIMATED',
  6: 'UNSIGNED_SRC',
  7: 'OVERRANGE',
  8: 'MIGRATED',
  9: 'SRC_RESTART',
  10: 'SRC_FW',
  11: 'REVERSE_LOST',
};

// Severity partition (contract §1): bits that push a value to BAD win over UNCERTAIN.
const BAD_BITS = [1, 7]; // TIME_UNSYNC (unresolved), OVERRANGE
const UNCERTAIN_BITS = [0, 2, 3, 4, 5, 6, 8, 9, 10, 11];

/** True if bit `n` is set in the quality mask. */
export function hasQualityBit(q: number | undefined, bit: number): boolean {
  return ((q ?? 0) & (1 << bit)) !== 0;
}

/** The names of all set bits, using the device legend when provided, else the canonical names. */
export function decodeQualityBits(q: number | undefined, legend: Record<number, string> = QUALITY_BIT_NAMES): string[] {
  const mask = q ?? 0;
  const names: string[] = [];
  for (let bit = 0; bit < 16; bit++) {
    if ((mask & (1 << bit)) !== 0) names.push(legend[bit] ?? `BIT_${bit}`);
  }
  return names;
}

/**
 * Classify a quality mask to good | uncertain | bad (contract §1 → OPC/Sparkplug mapping).
 * q=0 is good; TIME_UNSYNC/OVERRANGE are bad; the remaining documented bits are uncertain.
 */
export function classifyQuality(q: number | undefined): QualityLevel {
  const mask = q ?? 0;
  if (mask === 0) return 'good';
  if (BAD_BITS.some((b) => (mask & (1 << b)) !== 0)) return 'bad';
  if (UNCERTAIN_BITS.some((b) => (mask & (1 << b)) !== 0)) return 'uncertain';
  return 'uncertain'; // unknown bit set → be conservative
}

export type TimeQualityLevel = 'synced' | 'unsynced' | 'holdover';

/** Decode the telemetry `tq` field (0 unsynced · 1 synced · 2 holdover). */
export function classifyTimeQuality(tq: number | undefined): TimeQualityLevel {
  switch (tq) {
    case 1: return 'synced';
    case 2: return 'holdover';
    default: return 'unsynced';
  }
}

/** Short glyph for a time-quality indicator (spec §5.2: ✓ / ⚠ / ◔). */
export const TIME_QUALITY_GLYPH: Record<TimeQualityLevel, string> = {
  synced: '✓',
  unsynced: '⚠',
  holdover: '◔',
};
