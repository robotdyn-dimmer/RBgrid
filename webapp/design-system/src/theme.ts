import { createTheme, Theme } from '@mui/material/styles';
import { palettes, ThemeMode, tokens, type } from './tokens';

/**
 * RBgrid MUI theme — drop-in for design-system/src/theme.ts.
 * `rbgridTheme` stays (dark) for existing imports; new code uses createRbgridTheme(mode).
 */
export function createRbgridTheme(mode: ThemeMode = 'dark'): Theme {
  const p = palettes[mode];
  return createTheme({
    palette: {
      mode,
      background: { default: p.bg, paper: p.surface },
      primary: { main: p.primary, contrastText: p.onPrimary },
      success: { main: p.statusOnline },
      warning: { main: p.sevWarning },
      error: { main: p.sevCritical },
      info: { main: p.sevInfo },
      divider: p.border,
      text: { primary: p.ink, secondary: p.inkMuted },
    },
    shape: { borderRadius: tokens.radius.control },
    typography: {
      fontFamily: type.family.sans,
      fontSize: 14,
      h1: { ...type.scale.displayXl },
      h2: { ...type.scale.display },
      h3: { ...type.scale.metric },
      h4: { ...type.scale.title },
      h5: { ...type.scale.heading },
      h6: { ...type.scale.heading },
      body1: { ...type.scale.body },
      body2: { ...type.scale.bodySm },
      caption: { ...type.scale.label },
      button: { fontWeight: 500, textTransform: 'none' },
    },
    components: {
      MuiCssBaseline: {
        styleOverrides: {
          body: { fontVariantNumeric: 'tabular-nums', backgroundColor: p.bg },
          '@media (prefers-reduced-motion: reduce)': { '*': { animation: 'none !important', transition: 'none !important' } },
        },
      },
      MuiPaper: { styleOverrides: { root: { backgroundImage: 'none', border: `1px solid ${p.border}`, borderRadius: tokens.radius.card, boxShadow: 'none' } } },
      MuiButtonBase: { styleOverrides: { root: { '&.Mui-focusVisible': { outline: `2px solid ${p.focus}`, outlineOffset: 2 } } } },
      MuiPopover: { styleOverrides: { paper: { boxShadow: p.shadowPop } } },
      MuiTooltip: { styleOverrides: { tooltip: { backgroundColor: p.surface, color: p.ink, border: `1px solid ${p.border}`, boxShadow: p.shadowPop, fontSize: 12 } } },
    },
  });
}

export const rbgridTheme: Theme = createRbgridTheme('dark');
