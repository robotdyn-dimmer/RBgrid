/**
 * Mode-aware access to the design-system semantic tokens (flow, severity, series,
 * solar, status) that don't live on the MUI palette. Resolves against the active
 * MUI theme mode, so components stay correct in both dark and light themes.
 *
 *   const ds = useDsTokens();
 *   <Box sx={{ color: ds.flowExport }} />
 */
import { useTheme } from '@mui/material/styles';
import { DsPalette, dsPalettes, dsSeries, ThemeMode } from './dsTokens';

export function useDsTokens(): DsPalette {
  const theme = useTheme();
  const mode: ThemeMode = theme.palette.mode === 'light' ? 'light' : 'dark';
  return dsPalettes[mode];
}

/** The ordered comparison-series colours for the active theme (History charts). */
export function useDsSeries(): readonly string[] {
  const theme = useTheme();
  const mode: ThemeMode = theme.palette.mode === 'light' ? 'light' : 'dark';
  return dsSeries(mode);
}
