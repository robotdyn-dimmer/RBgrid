/**
 * @rbgrid/design-system — HA-style tokens + presentational components for RBGrid.
 * All components are pure (props-driven, no data fetching) so the design tool can
 * compose them freely.
 */
// Tokens: legacy `tokens`/`Tokens` (unchanged keys) + new palettes/type/motion/ThemeMode/Palette.
// Explicit named re-exports (NOT `export *`): the package is CJS, and webpack 5's named-export
// analysis reads the `Object.defineProperty` bindings these emit but NOT the runtime __exportStar.
export { tokens, palettes, motion, type } from './tokens';
export type { Tokens, ThemeMode, Palette } from './tokens';
// Theme: `rbgridTheme` (dark, legacy) + `createRbgridTheme(mode)`.
export { rbgridTheme, createRbgridTheme } from './theme';

export { RBGridThemeProvider } from './components/ThemeProvider';
export type { RBGridThemeProviderProps } from './components/ThemeProvider';

// — Surfaces & primitives —
export { Card } from './components/Card';
export type { CardProps } from './components/Card';

export { StatusChip } from './components/StatusChip';
export type { StatusChipProps, StatusKind } from './components/StatusChip';

export { StatTile } from './components/StatTile';
export type { StatTileProps } from './components/StatTile';

export { InfoField } from './components/InfoField';
export type { InfoFieldProps } from './components/InfoField';

export { SettingRow } from './components/SettingRow';
export type { SettingRowProps } from './components/SettingRow';

// — Entity / dashboard —
export { EntityCard } from './components/EntityCard';
export type { EntityCardProps, EntityMetric } from './components/EntityCard';

export { UnitCard } from './components/UnitCard';
export type { UnitCardProps } from './components/UnitCard';

export { SystemSummary } from './components/SystemSummary';
export type { SystemSummaryProps, SummaryStat } from './components/SystemSummary';

export { Gauge } from './components/Gauge';
export type { GaugeProps } from './components/Gauge';

export { TrendChart } from './components/TrendChart';
export type { TrendChartProps, TrendSeries, TrendBand } from './components/TrendChart';

// — Settings / forms —
export { PanelCard } from './components/PanelCard';
export type { PanelCardProps } from './components/PanelCard';

export { ToggleRow } from './components/ToggleRow';
export type { ToggleRowProps } from './components/ToggleRow';

export { FormField } from './components/FormField';
export type { FormFieldProps, FormFieldOption } from './components/FormField';

export { NetworkStatus } from './components/NetworkStatus';
export type { NetworkStatusProps } from './components/NetworkStatus';

export { ChannelCard } from './components/ChannelCard';
export type { ChannelCardProps } from './components/ChannelCard';

// — Layout —
export { AppShell } from './components/AppShell';
export type { AppShellProps, NavItem } from './components/AppShell';
