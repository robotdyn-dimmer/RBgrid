/**
 * Re-renders (remounts) the app on a language or number/date format switch: components call plain t(), so the
 * subtree is keyed by the active language. Switching is rare; local UI state resets.
 */
import React, { useSyncExternalStore } from 'react';
import { getFormat, getLang, getLanguages, onLangChange } from './i18n';

export const useLang = () => useSyncExternalStore(onLangChange, getLang, getLang);
export const useFormat = () => useSyncExternalStore(onLangChange, getFormat, getFormat);
/** Languages with a pack (repository index; English only until it is read). */
export const useLanguages = () => useSyncExternalStore(onLangChange, getLanguages, getLanguages);

export const I18nProvider: React.FC<{ children: React.ReactNode }> = ({ children }) => {
  const lang = useLang();
  const fmt = useFormat(); // a number/date format change re-renders every figure too
  return <React.Fragment key={`${lang}|${fmt}`}>{children}</React.Fragment>;
};
