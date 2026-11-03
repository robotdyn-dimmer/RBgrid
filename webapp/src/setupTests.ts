// jest-dom matchers (toBeInTheDocument, etc.). CRA loads this before each test file.
import '@testing-library/jest-dom';

// The app's built-in language is English; most tests read Russian texts — register the Russian
// pack (no network in tests) and make it active before each test file runs.
import ruCatalog from './i18n/locales/ru';
import { useCatalogForTests } from './i18n/i18n';
useCatalogForTests({ code: 'ru', label: 'Русский', intl: 'ru-RU', phrases: ruCatalog });
