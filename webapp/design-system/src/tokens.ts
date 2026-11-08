/**
 * RBgrid design tokens — drop-in for design-system/src/tokens.ts.
 * Source of truth: the RBgrid design system (tokens.json), 2026-10-12.
 * Backward compatible: `tokens.color.*` / `radius.*` / `space.*` / `font.*` keep their old keys
 * (dark values), so existing components compile unchanged. New code reads `palettes[mode]`.
 */
export type ThemeMode = 'dark' | 'light';

const dark = {
  bg: '#111418',
  surface: '#191d23',
  surfaceRaised: '#222831',
  border: '#2a2f3a',
  borderStrong: '#3b4250',
  ink: '#f9fafb',
  inkMuted: '#9ca3af',
  primary: '#60a5fa',
  onPrimary: '#0b1220',
  primarySoft: 'rgba(96,165,250,0.14)',
  focus: '#93c5fd',
  flowImport: '#a5b4c8',
  flowExport: '#2dd4bf',
  flowExportSoft: 'rgba(45,212,191,0.14)',
  solar: '#f59e0b',
  /** Solar as TEXT (values, shares) — the fill tone where it holds 4.5:1 on the surface. */
  solarInk: '#f59e0b',
  sevInfo: '#9ca3af',
  sevWarning: '#fbbf24',
  sevAlarm: '#fb923c',
  sevCritical: '#f87171',
  sevInfoSoft: 'rgba(156,163,175,0.14)',
  sevWarningSoft: 'rgba(251,191,36,0.13)',
  sevAlarmSoft: 'rgba(251,146,60,0.14)',
  sevCriticalSoft: 'rgba(248,113,113,0.14)',
  statusOnline: '#34d399',
  statusOffline: '#8a919e',
  // A tone deeper than the UI accents (charts audit 17.10, validate_palette dark/#191d23: L 0.48–0.67,
  // violet ↔ pink were ΔE 13 apart; now all checks pass on adjacent pairs).
  series: ['#3b82f6', '#0d9488', '#d97706', '#8b5cf6', '#ec4899', '#65a30d'],
  chartGrid: '#242a33',
  chartBand: 'rgba(96,165,250,0.18)',
  shadowPop: '0 8px 24px rgba(0,0,0,0.45)',
} as const;

/** A palette's shape: dark's keys with widened value types — both themes are checked against it
 *  (Kimi #22: `any` + `as Palette` let a wrong-typed light value through). */
type Widen<T> = T extends string ? string : T extends readonly (infer U)[] ? readonly Widen<U>[] : T;
export type Palette = { readonly [K in keyof typeof dark]: Widen<(typeof dark)[K]> };

const light: Palette = {
  bg: '#f3f5f8',
  surface: '#ffffff',
  surfaceRaised: '#f8fafc',
  border: '#dde2e9',
  borderStrong: '#a9b3c1',
  ink: '#111827',
  inkMuted: '#4b5563',
  primary: '#1d4ed8',
  onPrimary: '#ffffff',
  primarySoft: 'rgba(29,78,216,0.08)',
  focus: '#1d4ed8',
  flowImport: '#475569',
  // #0f766e read grey beside the blue (chroma 0.086 < 0.10, charts audit 17.10).
  flowExport: '#0d9488',
  flowExportSoft: 'rgba(13,148,136,0.08)',
  // Solar as a FILL: a clean amber (operator 20.10: #b45309 read as a muddy brown in the charts);
  // validate_palette light with primary + flowExport: all checks pass, 3.2:1 on white.
  solar: '#d97706',
  /** Solar as TEXT: the deeper amber keeps 4.5:1 on white. */
  solarInk: '#b45309',
  sevInfo: '#4b5563',
  sevWarning: '#a16207',
  sevAlarm: '#c2410c',
  sevCritical: '#b91c1c',
  sevInfoSoft: 'rgba(75,85,99,0.08)',
  sevWarningSoft: 'rgba(161,98,7,0.09)',
  sevAlarmSoft: 'rgba(194,65,12,0.08)',
  sevCriticalSoft: 'rgba(185,28,28,0.08)',
  statusOnline: '#047857',
  statusOffline: '#6b7280',
  series: ['#2563eb', '#0d9488', '#b45309', '#7e22ce', '#be185d', '#4d7c0f'],
  chartGrid: '#e8ecf1',
  chartBand: 'rgba(29,78,216,0.12)',
  shadowPop: '0 8px 24px rgba(17,24,39,0.12)',
};

export const palettes: Record<ThemeMode, Palette> = { dark, light };

export const type = {
  family: {
    sans: '"IBM Plex Sans", system-ui, -apple-system, "Segoe UI", sans-serif',
    mono: '"IBM Plex Mono", ui-monospace, "Cascadia Mono", Consolas, monospace',
  },
  /** Google Fonts URL — add as <link> in public/index.html (no other font host). */
  googleFontsHref:
    'https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;500&family=IBM+Plex+Sans:wght@400;500;600&display=swap',
  scale: {
    displayXl: { fontSize: 56, lineHeight: '60px', fontWeight: 600, letterSpacing: '-0.02em' },
    display: { fontSize: 36, lineHeight: '40px', fontWeight: 600, letterSpacing: '-0.01em' },
    metric: { fontSize: 22, lineHeight: '28px', fontWeight: 600 },
    title: { fontSize: 22, lineHeight: '28px', fontWeight: 600 },
    heading: { fontSize: 16, lineHeight: '22px', fontWeight: 600 },
    body: { fontSize: 14, lineHeight: '20px', fontWeight: 400 },
    bodySm: { fontSize: 13, lineHeight: '18px', fontWeight: 400 },
    label: { fontSize: 12, lineHeight: '16px', fontWeight: 500, letterSpacing: '0.02em' },
    mono: { fontSize: 12, lineHeight: '16px', fontWeight: 400 },
  },
} as const;

/** Motion (spec §2.3). No looping animation except EnergyFlowHome dots. */
export const motion = {
  valueTransitionMs: 700,
  easing: 'cubic-bezier(0.45, 0, 0.55, 1)',
  homeDotSeconds: { low: 6.5, mid: 4.8, high: 3.6 }, // <1 kW, <2.5 kW, >=2.5 kW
} as const;

/** Legacy shape (unchanged keys) + new scales. */
export const tokens = {
  color: {
    background: dark.bg,
    surface: dark.surface,
    surfaceAlt: dark.surfaceRaised,
    border: dark.border,
    primary: dark.primary,
    success: dark.statusOnline,
    warning: dark.sevWarning,
    error: dark.sevCritical,
    text: dark.ink,
    textSecondary: dark.inkMuted,
  },
  radius: { sm: 6, control: 8, card: 12, pill: 999 },
  space: { xs: 4, sm: 8, smd: 12, md: 16, lg: 24, xl: 32 },
  font: {
    family: type.family.sans,
    mono: type.family.mono,
    weight: { regular: 400, medium: 500, semibold: 600, bold: 600 },
  },
} as const;

export type Tokens = typeof tokens;
