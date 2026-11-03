/**
 * App-facing adapter over the design-system tokens. The design system owns the
 * values (UI-design spec §0: design system wins); this module just re-exports them
 * from `@rbgrid/design-system` so the app has ONE source. Token changes arrive as a
 * new version of the package's tokens.ts/theme.ts (design handoff), never hand-tuned here.
 *
 *   const ds = useDsTokens();            // mode-aware palette (see useDsTokens.ts)
 *   <Box sx={{ color: ds.flowExport }} />
 */
import { palettes, type as dsTypography, motion } from '@rbgrid/design-system';
import type { Palette, ThemeMode } from '@rbgrid/design-system';

export type { ThemeMode };
/** One theme's semantic palette (flow, severity×4, series, solar, status, surfaces, text). */
export type DsPalette = Palette;
/** A colour token key (every palette key except the `series` array). */
export type DsColorKey = Exclude<keyof Palette, 'series'>;

export const dsPalettes: Record<ThemeMode, Palette> = palettes;

/** Type scale (spec §2.1): displayXl/display/metric/title/heading/body/bodySm/label/mono. */
export const dsType = dsTypography.scale;
/** Font stacks: IBM Plex Sans + Plex Mono. */
export const dsFonts = dsTypography.family;
/** Motion params (spec §2.3): 700 ms value transition, easing, EnergyFlowHome dot cadences. */
export const dsMotion = motion;

/** The six comparison-series colours for a mode (History charts, up to 6 points). */
export const dsSeries = (mode: ThemeMode): readonly string[] => palettes[mode].series;

/** Resolve one colour token for a mode. */
export const dsToken = (mode: ThemeMode, key: DsColorKey): string => palettes[mode][key] as string;
