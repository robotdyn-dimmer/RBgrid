/**
 * UI constants for RBGrid WebApp
 * Colors, icons, themes, and visual configurations
 */

/**
 * Chart color palettes
 */
export const CHART_COLORS = {
  // Primary chart colors
  PRIMARY: [
    '#3b82f6', // Blue
    '#10b981', // Emerald
    '#f59e0b', // Amber
    '#ef4444', // Red
    '#8b5cf6', // Violet
    '#06b6d4', // Cyan
    '#f97316', // Orange
    '#84cc16', // Lime
    '#ec4899', // Pink
    '#6366f1', // Indigo
  ],
  
  // Gradient colors for area charts
  GRADIENTS: {
    BLUE: {
      start: 'rgba(59, 130, 246, 0.5)',
      end: 'rgba(59, 130, 246, 0.05)'
    },
    GREEN: {
      start: 'rgba(16, 185, 129, 0.5)',
      end: 'rgba(16, 185, 129, 0.05)'
    },
    AMBER: {
      start: 'rgba(245, 158, 11, 0.5)',
      end: 'rgba(245, 158, 11, 0.05)'
    },
    RED: {
      start: 'rgba(239, 68, 68, 0.5)',
      end: 'rgba(239, 68, 68, 0.05)'
    }
  },
  
  // Specific measurement colors
  MEASUREMENTS: {
    VOLTAGE: '#f59e0b',      // Amber
    CURRENT: '#3b82f6',      // Blue
    POWER: '#10b981',        // Emerald
    ENERGY: '#8b5cf6',       // Violet
    FREQUENCY: '#06b6d4',    // Cyan
    POWER_FACTOR: '#6366f1', // Indigo
  },
  
  // Heatmap colors
  HEATMAP: {
    LOW: '#10b981',          // Green
    MEDIUM: '#f59e0b',       // Amber
    HIGH: '#ef4444',         // Red
    CRITICAL: '#7c2d12',     // Dark red
  }
} as const;

/**
 * Icon mappings for device types and states
 */
export const ICON_MAPPINGS = {
  // Unit types
  UNIT_TYPES: {
    VOLTAGE_BUS: 'zap',
    CURRENT_LOAD: 'activity',
    POWER_METER: 'gauge',
    ENERGY_STORAGE: 'battery',
    SOLAR_PANEL: 'sun',
    WIND_TURBINE: 'wind',
  },
  
  // Unit roles
  UNIT_ROLES: {
    MAIN_SUPPLY: 'plug',
    ALTERNATIVE_SUPPLY: 'battery-charging',
    ZONE_LOAD: 'home',
    GENERATION: 'zap-off',
    STORAGE: 'battery',
  },
  
  // Sensor types
  SENSORS: {
    VOLTAGE: 'zap',
    CURRENT: 'activity',
    POWER: 'gauge',
    TEMPERATURE: 'thermometer',
    FREQUENCY: 'radio',
  },
  
  // System states
  STATES: {
    ONLINE: 'check-circle',
    OFFLINE: 'x-circle',
    WARNING: 'alert-triangle',
    ERROR: 'alert-circle',
    LOADING: 'loader',
    SYNCING: 'refresh-cw',
  },
  
  // Actions
  ACTIONS: {
    ADD: 'plus',
    EDIT: 'edit',
    DELETE: 'trash-2',
    SAVE: 'save',
    CANCEL: 'x',
    REFRESH: 'refresh-cw',
    DOWNLOAD: 'download',
    UPLOAD: 'upload',
    SETTINGS: 'settings',
    INFO: 'info',
    HELP: 'help-circle',
    SEARCH: 'search',
    FILTER: 'filter',
    SORT: 'arrow-up-down',
    EXPAND: 'maximize-2',
    COLLAPSE: 'minimize-2',
    FULLSCREEN: 'maximize',
    EXIT_FULLSCREEN: 'minimize',
  },
  
  // Navigation
  NAVIGATION: {
    DASHBOARD: 'layout-dashboard',
    DEVICES: 'cpu',
    STATISTICS: 'bar-chart-2',
    SETTINGS: 'settings',
    PROFILE: 'user',
    LOGOUT: 'log-out',
    MENU: 'menu',
  }
} as const;

/**
 * Status color mappings
 */
export const STATUS_COLORS = {
  // Connection status
  CONNECTION: {
    CONNECTED: '#10b981',    // Green
    DISCONNECTED: '#ef4444', // Red
    CONNECTING: '#f59e0b',   // Amber
    ERROR: '#dc2626',        // Red dark
    WARNING: '#f59e0b',      // Amber (same as CONNECTING)
    SUCCESS: '#10b981',      // Green (same as CONNECTED)
  },
  
  // Measurement status
  MEASUREMENT: {
    NORMAL: '#10b981',       // Green
    WARNING: '#f59e0b',      // Amber
    CRITICAL: '#ef4444',     // Red
    UNKNOWN: '#9ca3af',      // Gray
  },
  
  // Threshold indicators
  THRESHOLD: {
    SAFE: '#10b981',         // Green
    CAUTION: '#f59e0b',      // Amber
    DANGER: '#ef4444',       // Red
    EXCEEDED: '#dc2626',     // Red dark
  },
  
  // System health
  HEALTH: {
    HEALTHY: '#10b981',      // Green
    DEGRADED: '#f59e0b',     // Amber
    UNHEALTHY: '#ef4444',    // Red
    UNKNOWN: '#9ca3af',      // Gray
  }
} as const;

/**
 * Theme configurations
 */
// NOTE: values below are the approved design-system tokens (UI-design spec §2.1,
// artifact project/tokens.json). Full semantic palette (flow/severity/series/…) lives
// in theme/dsTokens.ts; THEME_CONFIG carries only the subset MUI's base palette needs.
export const THEME_CONFIG = {
  // Light theme — DS light values
  LIGHT: {
    PRIMARY: '#1d4ed8',       // ds primary (light)
    SECONDARY: '#0f766e',     // ds flow-export (light) — teal accent
    BACKGROUND: '#f3f5f8',    // ds bg (light)
    SURFACE: '#ffffff',       // ds surface (light)
    TEXT: {
      PRIMARY: '#111827',     // ds ink
      SECONDARY: '#4b5563',   // ds ink-muted
      DISABLED: '#9ca3af',
    },
    BORDER: '#dde2e9',        // ds border (light)
    DIVIDER: '#dde2e9',       // spec §2.1: divider = border token
  },

  // Dark theme — DS dark values (primary set kept from the current package per spec)
  DARK: {
    PRIMARY: '#60a5fa',       // ds primary (dark) — unchanged, from package
    SECONDARY: '#2dd4bf',     // ds flow-export (dark) — teal accent
    BACKGROUND: '#111418',    // ds bg (dark) — was #111827
    SURFACE: '#191d23',       // ds surface (dark) — was #1f2937
    TEXT: {
      PRIMARY: '#f9fafb',     // ds ink — unchanged
      SECONDARY: '#9ca3af',   // ds ink-muted — was #d1d5db
      DISABLED: '#6b7280',
    },
    BORDER: '#2a2f3a',        // ds border (dark) — was #374151
    DIVIDER: '#2a2f3a',       // spec §2.1: divider = border token
  }
} as const;

/**
 * Layout constants
 */
export const LAYOUT = {
  // Sidebar
  SIDEBAR: {
    WIDTH_EXPANDED: 260,
    WIDTH_COLLAPSED: 64,
    BREAKPOINT: 768,
  },
  
  // Header
  HEADER: {
    HEIGHT: 64,
    HEIGHT_MOBILE: 56,
  },
  
  // Content
  CONTENT: {
    MAX_WIDTH: 1200,
    PADDING: 24,
    PADDING_MOBILE: 16,
  },
  
  // Grid
  GRID: {
    COLUMNS: 12,
    GUTTER: 24,
    GUTTER_MOBILE: 16,
  },
  
  // Cards
  CARD: {
    PADDING: 16,
    PADDING_MOBILE: 16,
    RADIUS: 12,
    SHADOW: '0 1px 3px 0 rgb(0 0 0 / 0.1)',
  }
} as const;

/**
 * Animation durations (ms)
 */
export const ANIMATIONS = {
  FAST: 150,
  NORMAL: 300,
  SLOW: 500,
  CHART: 750,
  PAGE: 400,
} as const;

/**
 * Z-index layers
 */
export const Z_INDEX = {
  DROPDOWN: 1000,
  STICKY: 1020,
  FIXED: 1030,
  MODAL_BACKDROP: 1040,
  MODAL: 1050,
  POPOVER: 1060,
  TOOLTIP: 1070,
  NOTIFICATION: 1080,
} as const;

/**
 * Breakpoints for responsive design
 */
/**
 * Breakpoints from the UI spec §9 (review 03): phone < 420 ≤ large phone < 820 (bottom tab
 * bar, one column) ≤ tablet/desktop with side rail < 1100 (KPI ×3, points ×2) ≤ full layout
 * < 1440 ≤ wide (content capped at 1440).
 */
export const BREAKPOINTS = {
  XS: 0,
  SM: 420,
  MD: 820,
  LG: 1100,
  XL: 1440,
  XXL: 1920,
} as const;

/**
 * Font configurations
 */
export const FONTS = {
  // Font families — IBM Plex Sans / Plex Mono (UI-design spec §2.1 / D4, replaces Roboto)
  FAMILY: {
    SANS: '"IBM Plex Sans", system-ui, -apple-system, "Segoe UI", sans-serif',
    MONO: '"IBM Plex Mono", ui-monospace, "Cascadia Mono", Consolas, monospace',
  },
  
  // Font sizes
  SIZE: {
    XS: '0.75rem',    // 12px
    SM: '0.875rem',   // 14px
    BASE: '1rem',     // 16px
    LG: '1.125rem',   // 18px
    XL: '1.25rem',    // 20px
    '2XL': '1.5rem',  // 24px
    '3XL': '1.875rem', // 30px
    '4XL': '2.25rem', // 36px
  },
  
  // Font weights
  WEIGHT: {
    LIGHT: 300,
    NORMAL: 400,
    MEDIUM: 500,
    SEMIBOLD: 600,
    BOLD: 700,
  },
  
  // Line heights
  LINE_HEIGHT: {
    NONE: 1,
    TIGHT: 1.25,
    NORMAL: 1.5,
    RELAXED: 1.75,
    LOOSE: 2,
  }
} as const;

/**
 * Dashboard widget configurations
 */
export const DASHBOARD_WIDGETS = {
  // Widget sizes
  SIZES: {
    SMALL: { w: 3, h: 2 },
    MEDIUM: { w: 6, h: 3 },
    LARGE: { w: 9, h: 4 },
    FULL: { w: 12, h: 4 },
  },
  
  // Widget types
  TYPES: {
    METRIC: {
      minSize: { w: 3, h: 2 },
      defaultSize: { w: 3, h: 2 },
      resizable: false,
    },
    CHART: {
      minSize: { w: 6, h: 3 },
      defaultSize: { w: 6, h: 4 },
      resizable: true,
    },
    TABLE: {
      minSize: { w: 6, h: 4 },
      defaultSize: { w: 12, h: 6 },
      resizable: true,
    },
    GAUGE: {
      minSize: { w: 3, h: 3 },
      defaultSize: { w: 3, h: 3 },
      resizable: false,
    },
  }
} as const;

/**
 * Notification settings
 */
export const NOTIFICATIONS = {
  // Positions
  POSITION: {
    TOP_RIGHT: 'top-right',
    TOP_LEFT: 'top-left',
    BOTTOM_RIGHT: 'bottom-right',
    BOTTOM_LEFT: 'bottom-left',
  },
  
  // Durations (ms)
  DURATION: {
    SHORT: 3000,
    NORMAL: 5000,
    LONG: 8000,
    PERSISTENT: 0,
  },
  
  // Max visible
  MAX_VISIBLE: 5,
} as const;

/**
 * Table configurations
 */
export const TABLE = {
  // Page size options
  PAGE_SIZES: [10, 25, 50, 100],
  
  // Default page size
  DEFAULT_PAGE_SIZE: 25,
  
  // Row heights
  ROW_HEIGHT: {
    COMPACT: 36,
    NORMAL: 48,
    COMFORTABLE: 60,
  },
  
  // Column widths
  COLUMN_WIDTH: {
    ICON: 48,
    CHECKBOX: 48,
    ACTIONS: 120,
    MIN: 100,
    MAX: 500,
  }
} as const;

/**
 * Form configurations
 */
export const FORM = {
  // Field sizes
  FIELD_SIZE: {
    SMALL: 'small',
    MEDIUM: 'medium',
    LARGE: 'large',
  },
  
  // Label positions
  LABEL_POSITION: {
    TOP: 'top',
    LEFT: 'left',
    RIGHT: 'right',
  },
  
  // Validation display
  VALIDATION: {
    ON_BLUR: 'onBlur',
    ON_CHANGE: 'onChange',
    ON_SUBMIT: 'onSubmit',
  }
} as const;