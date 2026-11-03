/**
 * Theme Provider Component
 * Manages theme state and provides theme context to the application.
 *
 * The user's choice is a *preference* — 'system' | 'dark' | 'light' — persisted in
 * localStorage under THEME_STORAGE_KEY. 'system' (the default) follows
 * `prefers-color-scheme` live. `forcedTheme` (tests, kiosk) overrides everything and
 * is never persisted.
 */

import React, { createContext, useCallback, useContext, useEffect, useMemo, useState } from 'react';
import { ThemeProvider as MuiThemeProvider } from '@mui/material/styles';
import { CssBaseline } from '@mui/material';
import { lightTheme, darkTheme } from './index';

/** localStorage key of the theme preference ('system' | 'dark' | 'light'). */
export const THEME_STORAGE_KEY = 'rbgrid.theme';

export type ThemePreference = 'system' | 'dark' | 'light';

const isPreference = (v: unknown): v is ThemePreference => v === 'system' || v === 'dark' || v === 'light';

function readPreference(): ThemePreference {
  try {
    const v = window.localStorage.getItem(THEME_STORAGE_KEY);
    return isPreference(v) ? v : 'system';
  } catch {
    return 'system';
  }
}

function writePreference(p: ThemePreference): void {
  try {
    if (p === 'system') window.localStorage.removeItem(THEME_STORAGE_KEY);
    else window.localStorage.setItem(THEME_STORAGE_KEY, p);
  } catch {
    /* storage blocked — preference lives for this session only */
  }
}

const DARK_QUERY = '(prefers-color-scheme: dark)';

function systemPrefersDark(fallback: boolean): boolean {
  if (typeof window === 'undefined' || typeof window.matchMedia !== 'function') return fallback;
  return window.matchMedia(DARK_QUERY).matches;
}

/**
 * Theme context type
 */
interface ThemeContextType {
  /** Current (resolved) theme mode */
  isDarkMode: boolean;
  /** Toggle between light and dark themes (sets an explicit preference) */
  toggleTheme: () => void;
  /** Set theme mode explicitly (sets an explicit preference) */
  setTheme: (isDark: boolean) => void;
  /** The stored user preference ('system' follows prefers-color-scheme) */
  preference: ThemePreference;
  /** Change (and persist) the user preference */
  setPreference: (p: ThemePreference) => void;
  /** True when `forcedTheme` pins the mode (the switch has no effect) */
  forced: boolean;
  /** Current theme object */
  theme: typeof lightTheme;
}

/**
 * Theme context
 */
const ThemeContext = createContext<ThemeContextType | null>(null);

/**
 * Hook to use theme context
 * @throws Error if used outside of ThemeProvider
 */
export const useTheme = () => {
  const context = useContext(ThemeContext);
  if (!context) {
    throw new Error('useTheme must be used within ThemeProvider');
  }
  return context;
};

/**
 * Theme provider props
 */
interface ThemeProviderProps {
  /** Child components */
  children: React.ReactNode;
  /** Mode used for 'system' when the browser can't report prefers-color-scheme */
  defaultDarkMode?: boolean;
  /** Force theme mode (overrides user preference; not persisted) */
  forcedTheme?: 'light' | 'dark' | null;
}

/**
 * Theme Provider Component
 * Provides theme context and manages theme state
 */
export const ThemeProvider: React.FC<ThemeProviderProps> = ({
  children,
  defaultDarkMode = true,
  forcedTheme = null,
}) => {
  const [preference, setPreferenceState] = useState<ThemePreference>(readPreference);
  const [systemDark, setSystemDark] = useState<boolean>(() => systemPrefersDark(defaultDarkMode));

  // Follow the OS setting live (only matters while preference === 'system').
  useEffect(() => {
    if (typeof window === 'undefined' || typeof window.matchMedia !== 'function') return;
    const mq = window.matchMedia(DARK_QUERY);
    const onChange = (e: MediaQueryListEvent) => setSystemDark(e.matches);
    setSystemDark(mq.matches);
    if (mq.addEventListener) mq.addEventListener('change', onChange);
    else mq.addListener(onChange);
    return () => {
      if (mq.removeEventListener) mq.removeEventListener('change', onChange);
      else mq.removeListener(onChange);
    };
  }, []);

  // Sync the preference across tabs.
  useEffect(() => {
    const onStorage = (e: StorageEvent) => {
      if (e.key === THEME_STORAGE_KEY) setPreferenceState(isPreference(e.newValue) ? e.newValue : 'system');
    };
    window.addEventListener('storage', onStorage);
    return () => window.removeEventListener('storage', onStorage);
  }, []);

  const setPreference = useCallback((p: ThemePreference) => {
    setPreferenceState(p);
    writePreference(p);
  }, []);

  const isDarkMode = forcedTheme ? forcedTheme === 'dark' : preference === 'system' ? systemDark : preference === 'dark';

  const setTheme = useCallback((isDark: boolean) => setPreference(isDark ? 'dark' : 'light'), [setPreference]);
  const toggleTheme = useCallback(() => setPreference(isDarkMode ? 'light' : 'dark'), [setPreference, isDarkMode]);

  const theme = useMemo(() => (isDarkMode ? darkTheme : lightTheme), [isDarkMode]);

  const contextValue = useMemo<ThemeContextType>(
    () => ({ isDarkMode, toggleTheme, setTheme, preference, setPreference, forced: !!forcedTheme, theme }),
    [isDarkMode, toggleTheme, setTheme, preference, setPreference, forcedTheme, theme],
  );

  // Update document meta theme-color + the UA color-scheme (native scrollbars/controls).
  useEffect(() => {
    const metaThemeColor = document.querySelector('meta[name="theme-color"]');
    if (metaThemeColor) {
      metaThemeColor.setAttribute('content', theme.palette.background.default);
    }
    document.documentElement.style.colorScheme = isDarkMode ? 'dark' : 'light';
  }, [theme, isDarkMode]);

  return (
    <ThemeContext.Provider value={contextValue}>
      <MuiThemeProvider theme={theme}>
        <CssBaseline />
        {children}
      </MuiThemeProvider>
    </ThemeContext.Provider>
  );
};

/**
 * Higher-order component to inject theme props
 */
export function withTheme<P extends object>(
  Component: React.ComponentType<P & { theme: ThemeContextType }>
): React.FC<P> {
  return (props: P) => {
    const theme = useTheme();
    return <Component {...props} theme={theme} />;
  };
}
