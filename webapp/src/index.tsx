/**
 * RBGrid webapp entry point. Mounts the app into the device-served page (#app).
 */
import React from 'react';
import { createRoot } from 'react-dom/client';
import App from './App';
import { I18nProvider, initI18n } from './i18n';
import { DEMO_ONLY } from './services/connection';
import { applyDemoScreenLink } from './mock/demoVariant';

const container = document.getElementById('app');
if (!container) {
  throw new Error('RBGrid: root container #app not found');
}
const root = createRoot(container);
// The public demo opens the screen a site link names (`?scenario=…&screen=history&range=7d`).
if (DEMO_ONLY) applyDemoScreenLink();
// Activate the preferred language first (lazy chunk for non-default) — no flash of Russian.
void initI18n().finally(() => root.render(
  <React.StrictMode>
    <I18nProvider><App /></I18nProvider>
  </React.StrictMode>
));
