/**
 * Hook for localStorage management
 * Provides type-safe localStorage access with React state synchronization
 */

import { useState, useEffect, useCallback } from 'react';

/**
 * Local storage hook options
 */
export interface UseLocalStorageOptions {
  /** Whether to sync across tabs */
  syncTabs?: boolean;
  /** Custom serializer */
  serializer?: (value: any) => string;
  /** Custom deserializer */
  deserializer?: (value: string) => any;
}

/**
 * Check if localStorage is available
 */
const isLocalStorageAvailable = () => {
  try {
    return typeof window !== 'undefined' && 'localStorage' in window;
  } catch {
    return false;
  }
};

/**
 * Custom hook for localStorage
 */
export function useLocalStorage<T>(
  key: string,
  defaultValue: T,
  options: UseLocalStorageOptions = {}
): [T, (value: T | ((prev: T) => T)) => void, () => void] {
  const {
    syncTabs = true,
    serializer = JSON.stringify,
    deserializer = JSON.parse,
  } = options;

  // Initialize state with localStorage value or default
  const [storedValue, setStoredValue] = useState<T>(() => {
    if (!isLocalStorageAvailable()) {
      console.warn('localStorage is not available. Using default value.');
      return defaultValue;
    }
    
    try {
      const item = window.localStorage.getItem(key);
      return item ? deserializer(item) : defaultValue;
    } catch (error) {
      console.error(`Error reading localStorage key "${key}":`, error);
      return defaultValue;
    }
  });

  // Update localStorage when state changes
  const setValue = useCallback(
    (value: T | ((prev: T) => T)) => {
      try {
        // Allow value to be a function so we have same API as useState
        const valueToStore = value instanceof Function ? value(storedValue) : value;
        
        // Save state
        setStoredValue(valueToStore);
        
        // Save to localStorage
        window.localStorage.setItem(key, serializer(valueToStore));
        
        // Dispatch storage event for cross-tab sync
        if (syncTabs) {
          window.dispatchEvent(
            new StorageEvent('storage', {
              key,
              newValue: serializer(valueToStore),
              url: window.location.href,
              storageArea: window.localStorage,
            })
          );
        }
      } catch (error) {
        console.error(`Error setting localStorage key "${key}":`, error);
      }
    },
    [key, storedValue, serializer, syncTabs]
  );

  // Remove value from localStorage
  const removeValue = useCallback(() => {
    try {
      window.localStorage.removeItem(key);
      setStoredValue(defaultValue);
      
      // Dispatch storage event for cross-tab sync
      if (syncTabs) {
        window.dispatchEvent(
          new StorageEvent('storage', {
            key,
            newValue: null,
            url: window.location.href,
            storageArea: window.localStorage,
          })
        );
      }
    } catch (error) {
      console.error(`Error removing localStorage key "${key}":`, error);
    }
  }, [key, defaultValue, syncTabs]);

  // Listen for changes in other tabs
  useEffect(() => {
    if (!syncTabs) return;

    const handleStorageChange = (e: StorageEvent) => {
      if (e.key === key && e.storageArea === window.localStorage) {
        try {
          const newValue = e.newValue ? deserializer(e.newValue) : defaultValue;
          setStoredValue(newValue);
        } catch (error) {
          console.error(`Error syncing localStorage key "${key}":`, error);
        }
      }
    };

    window.addEventListener('storage', handleStorageChange);
    return () => window.removeEventListener('storage', handleStorageChange);
  }, [key, defaultValue, deserializer, syncTabs]);

  return [storedValue, setValue, removeValue];
}

/**
 * Hook for user preferences storage
 */
export function useUserPreferences() {
  const [preferences, setPreferences] = useLocalStorage('rbgrid_user_preferences', {
    theme: 'light',
    language: 'EN',
    units: {
      temperature: 'celsius',
      power: 'watts',
      energy: 'kwh',
    },
    display: {
      showGridLines: true,
      animationsEnabled: true,
      compactMode: false,
      autoRefresh: true,
      refreshInterval: 5000,
    },
    notifications: {
      enabled: true,
      sound: false,
      desktop: true,
      alertTypes: {
        error: true,
        warning: true,
        info: false,
      },
    },
    dashboard: {
      layout: 'default',
      widgets: ['power', 'voltage', 'current', 'energy'],
      chartTimeRange: '24h',
    },
  });

  const updatePreference = useCallback(
    <K extends keyof typeof preferences>(
      category: K,
      value: typeof preferences[K]
    ) => {
      setPreferences(prev => ({
        ...prev,
        [category]: value,
      }));
    },
    [setPreferences]
  );

  const resetPreferences = useCallback(() => {
    setPreferences(prev => ({
      theme: 'light',
      language: 'EN',
      units: {
        temperature: 'celsius',
        power: 'watts',
        energy: 'kwh',
      },
      display: {
        showGridLines: true,
        animationsEnabled: true,
        compactMode: false,
        autoRefresh: true,
        refreshInterval: 5000,
      },
      notifications: {
        enabled: true,
        sound: false,
        desktop: true,
        alertTypes: {
          error: true,
          warning: true,
          info: false,
        },
      },
      dashboard: {
        layout: 'default',
        widgets: ['power', 'voltage', 'current', 'energy'],
        chartTimeRange: '24h',
      },
    }));
  }, [setPreferences]);

  return {
    preferences,
    updatePreference,
    resetPreferences,
  };
}

/**
 * Hook for filter persistence
 */
export function usePersistedFilters<T extends Record<string, any>>(
  key: string,
  defaultFilters: T
) {
  const [filters, setFilters] = useLocalStorage(
    `rbgrid_filters_${key}`,
    defaultFilters
  );

  const updateFilter = useCallback(
    (filterKey: keyof T, value: any) => {
      setFilters(prev => ({
        ...prev,
        [filterKey]: value,
      }));
    },
    [setFilters]
  );

  const resetFilters = useCallback(() => {
    setFilters(defaultFilters);
  }, [setFilters, defaultFilters]);

  const hasActiveFilters = Object.entries(filters).some(
    ([key, value]) => value !== defaultFilters[key as keyof T]
  );

  return {
    filters,
    updateFilter,
    resetFilters,
    hasActiveFilters,
  };
}

/**
 * Hook for recent items storage
 */
export function useRecentItems<T extends { id: string | number }>(
  key: string,
  maxItems: number = 10
) {
  const [items, setItems] = useLocalStorage<T[]>(
    `rbgrid_recent_${key}`,
    []
  );

  const addItem = useCallback(
    (item: T) => {
      setItems(prev => {
        // Remove duplicate if exists
        const filtered = prev.filter(i => i.id !== item.id);
        // Add to beginning and limit size
        return [item, ...filtered].slice(0, maxItems);
      });
    },
    [setItems, maxItems]
  );

  const removeItem = useCallback(
    (id: string | number) => {
      setItems(prev => prev.filter(item => item.id !== id));
    },
    [setItems]
  );

  const clearItems = useCallback(() => {
    setItems([]);
  }, [setItems]);

  return {
    items,
    addItem,
    removeItem,
    clearItems,
  };
}

/**
 * Hook for session data storage
 */
export function useSessionData<T>(
  key: string,
  defaultValue: T
): [T, (value: T | ((prev: T) => T)) => void, () => void] {
  // Use sessionStorage instead of localStorage
  const [storedValue, setStoredValue] = useState<T>(() => {
    try {
      const item = window.sessionStorage.getItem(key);
      return item ? JSON.parse(item) : defaultValue;
    } catch (error) {
      console.error(`Error reading sessionStorage key "${key}":`, error);
      return defaultValue;
    }
  });

  const setValue = useCallback(
    (value: T | ((prev: T) => T)) => {
      try {
        const valueToStore = value instanceof Function ? value(storedValue) : value;
        setStoredValue(valueToStore);
        window.sessionStorage.setItem(key, JSON.stringify(valueToStore));
      } catch (error) {
        console.error(`Error setting sessionStorage key "${key}":`, error);
      }
    },
    [key, storedValue]
  );

  const removeValue = useCallback(() => {
    try {
      window.sessionStorage.removeItem(key);
      setStoredValue(defaultValue);
    } catch (error) {
      console.error(`Error removing sessionStorage key "${key}":`, error);
    }
  }, [key, defaultValue]);

  return [storedValue, setValue, removeValue];
}