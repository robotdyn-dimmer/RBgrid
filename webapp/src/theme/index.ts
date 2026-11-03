/**
 * Material-UI Theme Configuration
 * Defines light and dark themes for RBGrid WebApp
 */

import { createTheme, Theme, alpha } from '@mui/material/styles';
import { palettes as dsPalettes } from '@rbgrid/design-system';
import { 
  THEME_CONFIG, 
  FONTS, 
  BREAKPOINTS, 
  CHART_COLORS,
  STATUS_COLORS,
  ANIMATIONS,
  LAYOUT 
} from '../constants/ui';

/**
 * Common theme configuration shared between light and dark themes
 */
const commonThemeConfig = {
  typography: {
    fontFamily: FONTS.FAMILY.SANS,
    h1: {
      fontSize: FONTS.SIZE['4XL'],
      fontWeight: FONTS.WEIGHT.BOLD,
      lineHeight: FONTS.LINE_HEIGHT.TIGHT,
    },
    h2: {
      fontSize: FONTS.SIZE['3XL'],
      fontWeight: FONTS.WEIGHT.SEMIBOLD,
      lineHeight: FONTS.LINE_HEIGHT.TIGHT,
    },
    h3: {
      fontSize: FONTS.SIZE['2XL'],
      fontWeight: FONTS.WEIGHT.SEMIBOLD,
      lineHeight: FONTS.LINE_HEIGHT.NORMAL,
    },
    h4: {
      fontSize: FONTS.SIZE.XL,
      fontWeight: FONTS.WEIGHT.MEDIUM,
      lineHeight: FONTS.LINE_HEIGHT.NORMAL,
    },
    h5: {
      fontSize: FONTS.SIZE.LG,
      fontWeight: FONTS.WEIGHT.MEDIUM,
      lineHeight: FONTS.LINE_HEIGHT.NORMAL,
    },
    h6: {
      fontSize: FONTS.SIZE.BASE,
      fontWeight: FONTS.WEIGHT.MEDIUM,
      lineHeight: FONTS.LINE_HEIGHT.NORMAL,
    },
    body1: {
      fontSize: FONTS.SIZE.BASE,
      fontWeight: FONTS.WEIGHT.NORMAL,
      lineHeight: FONTS.LINE_HEIGHT.NORMAL,
    },
    body2: {
      fontSize: FONTS.SIZE.SM,
      fontWeight: FONTS.WEIGHT.NORMAL,
      lineHeight: FONTS.LINE_HEIGHT.NORMAL,
    },
    button: {
      fontSize: FONTS.SIZE.SM,
      fontWeight: FONTS.WEIGHT.MEDIUM,
      textTransform: 'none' as const,
    },
    caption: {
      fontSize: FONTS.SIZE.XS,
      fontWeight: FONTS.WEIGHT.NORMAL,
      lineHeight: FONTS.LINE_HEIGHT.NORMAL,
    },
    overline: {
      fontSize: FONTS.SIZE.XS,
      fontWeight: FONTS.WEIGHT.SEMIBOLD,
      textTransform: 'uppercase' as const,
      letterSpacing: '0.1em',
    },
  },
  breakpoints: {
    values: {
      xs: BREAKPOINTS.XS,
      sm: BREAKPOINTS.SM,
      md: BREAKPOINTS.MD,
      lg: BREAKPOINTS.LG,
      xl: BREAKPOINTS.XL,
      xxl: BREAKPOINTS.XXL, // custom breakpoint (declared in theme/types.ts BreakpointOverrides)
    },
  },
  shape: {
    // Base radius UNIT, not a card radius: MUI multiplies numeric sx borderRadius by this,
    // so with 12 a card's `borderRadius: 3` became 36 px (review 03 §2.1). Unit 4 → 3 = 12 px
    // card, 2 = 8 px control, 1.5 = 6 px chip (spec §2 radii 6/8/12).
    borderRadius: 4,
  },
  transitions: {
    duration: {
      shortest: ANIMATIONS.FAST,
      shorter: 200,
      short: ANIMATIONS.NORMAL,
      standard: ANIMATIONS.NORMAL,
      complex: 375,
      enteringScreen: 225,
      leavingScreen: 195,
    },
  },
};

/**
 * Light theme configuration
 */
export const lightTheme = createTheme({
  ...commonThemeConfig,
  palette: {
    mode: 'light',
    primary: {
      main: THEME_CONFIG.LIGHT.PRIMARY,
      light: alpha(THEME_CONFIG.LIGHT.PRIMARY, 0.2),
      dark: '#1e40af',
      contrastText: '#ffffff',
    },
    secondary: {
      main: THEME_CONFIG.LIGHT.SECONDARY,
      light: alpha(THEME_CONFIG.LIGHT.SECONDARY, 0.2),
      dark: '#115e59',
      contrastText: '#ffffff',
    },
    background: {
      default: THEME_CONFIG.LIGHT.BACKGROUND,
      paper: THEME_CONFIG.LIGHT.SURFACE,
    },
    text: {
      primary: THEME_CONFIG.LIGHT.TEXT.PRIMARY,
      secondary: THEME_CONFIG.LIGHT.TEXT.SECONDARY,
      disabled: THEME_CONFIG.LIGHT.TEXT.DISABLED,
    },
    divider: THEME_CONFIG.LIGHT.DIVIDER,
    // Status colours = DS light severity/status tokens: the 500-level CONNECTION colours
    // (#f59e0b / #10b981) are ~2:1 on white — unreadable as text/outlined chips (spec §9 ≥ 4.5:1).
    error: {
      main: dsPalettes.light.sevCritical,
      light: alpha(dsPalettes.light.sevCritical, 0.2),
      dark: '#991b1b',
      contrastText: '#ffffff',
    },
    warning: {
      main: dsPalettes.light.sevWarning,
      light: alpha(dsPalettes.light.sevWarning, 0.2),
      dark: '#854d0e',
      contrastText: '#ffffff',
    },
    info: {
      main: THEME_CONFIG.LIGHT.PRIMARY,
      light: alpha(THEME_CONFIG.LIGHT.PRIMARY, 0.2),
      dark: '#1e40af',
      contrastText: '#ffffff',
    },
    success: {
      main: dsPalettes.light.statusOnline,
      light: alpha(dsPalettes.light.statusOnline, 0.2),
      dark: '#065f46',
      contrastText: '#ffffff',
    },
    action: {
      selected: dsPalettes.light.primarySoft,
    },
    grey: {
      50: '#f9fafb',
      100: '#f3f4f6',
      200: '#e5e7eb',
      300: '#d1d5db',
      400: '#9ca3af',
      500: '#6b7280',
      600: '#4b5563',
      700: '#374151',
      800: '#1f2937',
      900: '#111827',
    },
  },
  components: {
    // Visible keyboard focus everywhere (spec §9: 2 px `focus` token, offset 2 px).
    MuiButtonBase: {
      styleOverrides: {
        root: {
          '&.Mui-focusVisible': {
            outline: `2px solid ${dsPalettes.light.focus}`,
            outlineOffset: 2,
          },
        },
      },
    },
    MuiCssBaseline: {
      styleOverrides: {
        // Tabular figures everywhere (spec §2.1): live numbers never change width on update.
        'html, body, input, button, textarea, select, .tnum': {
          fontVariantNumeric: 'tabular-nums',
          fontFeatureSettings: '"tnum" 1',
        },
        body: { fontFamily: FONTS.FAMILY.SANS },
        // Respect reduced motion (spec D3 / §10): no transitions or animations.
        '@media (prefers-reduced-motion: reduce)': {
          '*, *::before, *::after': {
            animationDuration: '0.001ms !important',
            animationIterationCount: '1 !important',
            transitionDuration: '0.001ms !important',
            scrollBehavior: 'auto !important',
          },
        },
      },
    },
    MuiButton: {
      styleOverrides: {
        root: {
          borderRadius: LAYOUT.CARD.RADIUS / 2,
          textTransform: 'none' as const,
          fontWeight: FONTS.WEIGHT.MEDIUM,
          padding: '8px 16px',
          transition: `all ${ANIMATIONS.FAST}ms ease-in-out`,
        },
        sizeLarge: {
          padding: '12px 24px',
          fontSize: FONTS.SIZE.BASE,
        },
        sizeSmall: {
          padding: '6px 12px',
          fontSize: FONTS.SIZE.XS,
          '@media (pointer: coarse)': { minHeight: 44 }, // touch target (HEU-24)
        },
      },
    },
    MuiCard: {
      styleOverrides: {
        root: {
          borderRadius: LAYOUT.CARD.RADIUS,
          boxShadow: LAYOUT.CARD.SHADOW,
          backgroundImage: 'none',
          border: `1px solid ${THEME_CONFIG.LIGHT.BORDER}`,
        },
      },
    },
    MuiCardContent: {
      styleOverrides: {
        root: {
          padding: LAYOUT.CARD.PADDING,
          '&:last-child': {
            paddingBottom: LAYOUT.CARD.PADDING,
          },
        },
      },
    },
    MuiChip: {
      styleOverrides: {
        root: {
          borderRadius: 6,
          fontWeight: FONTS.WEIGHT.MEDIUM,
          '&.Mui-focusVisible': { outline: `2px solid ${dsPalettes.light.focus}`, outlineOffset: 2 },
          '@media (pointer: coarse)': { minHeight: 36 },
        },
      },
    },
    MuiPaper: {
      styleOverrides: {
        root: {
          backgroundImage: 'none',
        },
        rounded: {
          borderRadius: LAYOUT.CARD.RADIUS,
        },
      },
    },
    MuiTextField: {
      styleOverrides: {
        root: {
          '& .MuiOutlinedInput-root': {
            borderRadius: 8,
          },
        },
      },
    },
    MuiTooltip: {
      styleOverrides: {
        tooltip: {
          borderRadius: 6,
          fontSize: FONTS.SIZE.XS,
          fontWeight: FONTS.WEIGHT.NORMAL,
        },
      },
    },
    // UX audit W3-4 (HEU-24/25): a visible focus ring on the segment toggles, tabs, chips and
    // links (only nav buttons and Buttons had one), and ≥ 44 px touch targets on touch screens.
    MuiToggleButton: {
      styleOverrides: {
        root: {
          '&.Mui-focusVisible': { outline: `2px solid ${dsPalettes.light.focus}`, outlineOffset: -2, zIndex: 1 },
          '@media (pointer: coarse)': { minHeight: 44 },
        },
      },
    },
    MuiTab: {
      styleOverrides: {
        root: { '&.Mui-focusVisible': { outline: `2px solid ${dsPalettes.light.focus}`, outlineOffset: -2 }, '@media (pointer: coarse)': { minHeight: 44 } },
      },
    },
    MuiLink: {
      styleOverrides: {
        root: { '&:focus-visible': { outline: `2px solid ${dsPalettes.light.focus}`, outlineOffset: 2, borderRadius: 2 } },
      },
    },
    MuiAlert: {
      styleOverrides: {
        // MUI derives the standard Alert's text colour from `palette.X.light`, which here is a
        // 20 % alpha tint (used as a soft fill) → the message rendered at 0.2 opacity (UX audit T1).
        // Pin text/fill per severity instead: light mode = the 700-level `dark` ink on a 10 % tint,
        // dark mode = the `main` ink on a 16 % tint; ≥ 4.5:1 in both.
        root: ({ ownerState, theme }: { ownerState: { severity?: string; variant?: string }; theme: Theme }) => {
          const sev = (ownerState.severity ?? 'success') as 'error' | 'warning' | 'info' | 'success';
          const pal = theme.palette[sev];
          const dark = theme.palette.mode === 'dark';
          return {
            borderRadius: 8,
            ...(ownerState.variant === 'standard' || !ownerState.variant ? {
              color: dark ? pal.main : pal.dark,
              backgroundColor: alpha(pal.main, dark ? 0.16 : 0.1),
              '& .MuiAlert-icon': { color: dark ? pal.main : pal.dark },
            } : {}),
          };
        },
      },
    },
  },
});

/**
 * Dark theme configuration
 */
export const darkTheme = createTheme({
  ...commonThemeConfig,
  palette: {
    mode: 'dark',
    primary: {
      main: THEME_CONFIG.DARK.PRIMARY,
      light: alpha(THEME_CONFIG.DARK.PRIMARY, 0.2),
      dark: '#3b82f6',
      contrastText: '#111827',
    },
    secondary: {
      main: THEME_CONFIG.DARK.SECONDARY,
      light: alpha(THEME_CONFIG.DARK.SECONDARY, 0.2),
      dark: '#10b981',
      contrastText: '#111827',
    },
    background: {
      default: THEME_CONFIG.DARK.BACKGROUND,
      paper: THEME_CONFIG.DARK.SURFACE,
    },
    text: {
      primary: THEME_CONFIG.DARK.TEXT.PRIMARY,
      secondary: THEME_CONFIG.DARK.TEXT.SECONDARY,
      disabled: THEME_CONFIG.DARK.TEXT.DISABLED,
    },
    divider: THEME_CONFIG.DARK.DIVIDER,
    action: {
      selected: dsPalettes.dark.primarySoft,
    },
    error: {
      main: '#f87171',
      light: alpha('#f87171', 0.2),
      dark: '#ef4444',
    },
    warning: {
      main: '#fbbf24',
      light: alpha('#fbbf24', 0.2),
      dark: '#f59e0b',
    },
    info: {
      main: THEME_CONFIG.DARK.PRIMARY,
      light: alpha(THEME_CONFIG.DARK.PRIMARY, 0.2),
      dark: '#3b82f6',
    },
    success: {
      main: '#34d399',
      light: alpha('#34d399', 0.2),
      dark: '#10b981',
    },
    grey: {
      50: '#111827',
      100: '#1f2937',
      200: '#374151',
      300: '#4b5563',
      400: '#6b7280',
      500: '#9ca3af',
      600: '#d1d5db',
      700: '#e5e7eb',
      800: '#f3f4f6',
      900: '#f9fafb',
    },
  },
  components: {
    ...lightTheme.components,
    MuiButtonBase: {
      styleOverrides: {
        root: {
          '&.Mui-focusVisible': {
            outline: `2px solid ${dsPalettes.dark.focus}`,
            outlineOffset: 2,
          },
        },
      },
    },
    MuiCard: {
      styleOverrides: {
        root: {
          borderRadius: LAYOUT.CARD.RADIUS,
          boxShadow: '0 1px 3px 0 rgb(0 0 0 / 0.3)',
          backgroundImage: 'none',
          border: `1px solid ${THEME_CONFIG.DARK.BORDER}`,
        },
      },
    },
  },
});

/**
 * Export chart colors and status colors for use in components
 */
export { CHART_COLORS, STATUS_COLORS };

/**
 * Export theme type for TypeScript
 */
export type AppTheme = typeof lightTheme;