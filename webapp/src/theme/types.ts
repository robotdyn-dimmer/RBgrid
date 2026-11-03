/**
 * Theme type definitions
 * Extends Material-UI theme with custom properties
 */

import { Theme } from '@mui/material/styles';
import { CHART_COLORS, STATUS_COLORS } from '../constants/ui';

/**
 * Chart colors type
 */
interface ChartColors {
  primary: string[];
  gradients: typeof CHART_COLORS.GRADIENTS;
  measurements: typeof CHART_COLORS.MEASUREMENTS;
  heatmap: typeof CHART_COLORS.HEATMAP;
}

/**
 * Status colors type
 */
interface StatusColors {
  connection: typeof STATUS_COLORS.CONNECTION;
  measurement: typeof STATUS_COLORS.MEASUREMENT;
  threshold: typeof STATUS_COLORS.THRESHOLD;
  health: typeof STATUS_COLORS.HEALTH;
}

/**
 * Custom spacing values
 */
interface CustomSpacing {
  card: number;
  section: number;
  widget: number;
}

/**
 * Module augmentation for Material-UI theme
 */
declare module '@mui/material/styles' {
  interface Theme {
    chart: ChartColors;
    status: StatusColors;
    customSpacing: CustomSpacing;
  }
  
  interface ThemeOptions {
    chart?: ChartColors;
    status?: StatusColors;
    customSpacing?: CustomSpacing;
  }
  
  interface Palette {
    chart: {
      primary: string[];
      status: {
        success: string;
        warning: string;
        error: string;
        info: string;
      };
    };
  }
  
  interface PaletteOptions {
    chart?: {
      primary?: string[];
      status?: {
        success?: string;
        warning?: string;
        error?: string;
        info?: string;
      };
    };
  }
  
  interface TypeBackground {
    paper: string;
    default: string;
    light?: string;
    dark?: string;
  }
}

/**
 * Component variant definitions
 */
declare module '@mui/material/Button' {
  interface ButtonPropsVariantOverrides {
    gradient: true;
  }
}

declare module '@mui/material/Card' {
  interface CardPropsVariantOverrides {
    dashboard: true;
    widget: true;
  }
}

declare module '@mui/material/Paper' {
  interface PaperPropsVariantOverrides {
    dashboard: true;
    widget: true;
  }
}

/**
 * Custom breakpoint
 */
declare module '@mui/material/styles' {
  interface BreakpointOverrides {
    xs: true;
    sm: true;
    md: true;
    lg: true;
    xl: true;
    xxl: true; // Custom breakpoint
  }
}

/**
 * Theme mode type
 */
export type ThemeMode = 'light' | 'dark' | 'auto';

/**
 * Theme preferences type
 */
export interface ThemePreferences {
  mode: ThemeMode;
  primaryColor?: string;
  secondaryColor?: string;
  fontSize?: 'small' | 'medium' | 'large';
  borderRadius?: number;
  density?: 'comfortable' | 'standard' | 'compact';
}

/**
 * Export augmented theme type
 */
export type RBGridTheme = Theme;