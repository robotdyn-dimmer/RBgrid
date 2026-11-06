/**
 * The grid input as ONE history/economics series. A STD-U3I3 input metered per phase is three
 * MAIN_SUPPLY points; the hub (fw 1.1.6, `capabilities.inputAggregate`) answers `unit=input` on
 * /history and /economics with their records summed by `ts` (root input-aggregate-api 06.11).
 * Older hubs: the main point's own HEX4 (L1 only on a split input). Energy/economics only —
 * `type=electrical` is not aggregated (400), read the points for that.
 */
import { useCapabilities } from '../core/react/DeviceProvider';
import type { DeviceCapabilities } from '../core/schema/capabilities';

export const INPUT_SERIES = 'input';

export function inputSeriesId(caps: DeviceCapabilities, inputHex: string | null): string | null {
  return inputHex && caps.flags.inputAggregate ? INPUT_SERIES : inputHex;
}

/** The input's history/economics id: `input` where the hub sums it, else its HEX4. */
export function useInputSeries(inputHex: string | null): string | null {
  return inputSeriesId(useCapabilities(), inputHex);
}
