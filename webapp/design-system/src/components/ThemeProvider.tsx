import React from 'react';
import { ThemeProvider as MuiThemeProvider, CssBaseline } from '@mui/material';
import { rbgridTheme } from '../theme';

export interface RBGridThemeProviderProps {
  /** Wrapped content. */
  children?: React.ReactNode;
}

/**
 * Wraps content in the RBGrid HA-dark MUI theme. Used at the app root and as the
 * design-tool preview provider so every component renders on the real brand.
 */
export const RBGridThemeProvider: React.FC<RBGridThemeProviderProps> = ({ children }) => (
  <MuiThemeProvider theme={rbgridTheme}>
    <CssBaseline />
    {children}
  </MuiThemeProvider>
);
