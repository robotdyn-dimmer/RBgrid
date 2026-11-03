import {
  classifyQuality, classifyTimeQuality, decodeQualityBits, hasQualityBit, QUALITY_BIT_NAMES,
} from './quality';

describe('quality bitmask (contract v4 §1)', () => {
  it('q=0 is good', () => {
    expect(classifyQuality(0)).toBe('good');
    expect(classifyQuality(undefined)).toBe('good');
  });

  it('PARTIAL (bit0) alone is uncertain', () => {
    expect(classifyQuality(1 << 0)).toBe('uncertain');
  });

  it('TIME_UNSYNC (bit1) and OVERRANGE (bit7) are bad', () => {
    expect(classifyQuality(1 << 1)).toBe('bad');
    expect(classifyQuality(1 << 7)).toBe('bad');
  });

  it('bad wins over uncertain when both kinds of bits are set', () => {
    expect(classifyQuality((1 << 0) | (1 << 1))).toBe('bad'); // PARTIAL + TIME_UNSYNC
  });

  it('RECOMPUTED/MIGRATED/SRC_GAP are uncertain', () => {
    expect(classifyQuality(1 << 4)).toBe('uncertain');
    expect(classifyQuality(1 << 8)).toBe('uncertain');
    expect(classifyQuality(1 << 3)).toBe('uncertain');
  });

  it('hasQualityBit reads individual bits', () => {
    expect(hasQualityBit((1 << 3) | (1 << 5), 3)).toBe(true);
    expect(hasQualityBit((1 << 3) | (1 << 5), 4)).toBe(false);
  });

  it('decodeQualityBits names set bits with the canonical legend', () => {
    expect(decodeQualityBits((1 << 0) | (1 << 8))).toEqual(['PARTIAL', 'MIGRATED']);
    expect(decodeQualityBits(0)).toEqual([]);
  });

  it('decodeQualityBits honors a device legend override', () => {
    const legend = { ...QUALITY_BIT_NAMES, 0: 'PART' };
    expect(decodeQualityBits(1 << 0, legend)).toEqual(['PART']);
  });
});

describe('time quality tq', () => {
  it('maps 0/1/2 to unsynced/synced/holdover', () => {
    expect(classifyTimeQuality(0)).toBe('unsynced');
    expect(classifyTimeQuality(1)).toBe('synced');
    expect(classifyTimeQuality(2)).toBe('holdover');
    expect(classifyTimeQuality(undefined)).toBe('unsynced');
  });
});
