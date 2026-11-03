/**
 * Smoke tests — render the device-management-stack pages within the providers and a
 * mock transport, asserting they mount without crashing. Catches the class of runtime
 * errors (bad hook order, undefined access, missing provider) that tsc + the unit tests
 * don't. Not behavioural — just "does it render".
 */
import React from 'react';
import { render, screen, fireEvent } from '@testing-library/react';
import { MemoryRouter, Routes, Route } from 'react-router-dom';
import { ThemeProvider } from '../theme/ThemeProvider';
import {
  DeviceProvider, DeviceTransport,
  mockSchema, mockProvisioning, mockConfig, mockEconomics, mockSummary, mockEspnow,
} from '../core';
import { connectDemo, disconnect } from '../services/connection';
import { ConfigurationPage } from './ConfigurationPage';
import { EconomicsPage } from './EconomicsPage';
import { DevicesPage } from './DevicesPage';
import { DashboardPage } from './DashboardPage';
import { PointDetailPage } from './PointDetailPage';
import { OnboardingPage } from './OnboardingPage';
import { HistoryPage } from './HistoryPage';
import { EventsPage } from './EventsPage';
import { TariffsPage } from './TariffsPage';
import { ConnectScreen } from './ConnectScreen';

// Demo mode so the data hooks (useSensors/useHistory) return synthetic data and the
// pages reach their loaded state instead of hanging on a (serverless) network call.
beforeAll(() => connectDemo());
afterAll(() => disconnect());

/** A mock transport returning the canned shapes (no network); optional per-domain overrides. */
function fakeTransport(over: Partial<Record<string, Record<string, unknown>>> = {}): DeviceTransport {
  return {
    kind: 'direct',
    getSchema: async () => mockSchema(),
    getConfig: async (d) => ({ ...mockConfig(d), ...(over[d] ?? {}) } as ReturnType<typeof mockConfig>),
    setConfig: async () => undefined,
    getProvisioning: async () => mockProvisioning(),
    getEconomics: async (u) => mockEconomics(u),
    getSummary: async (u, p) => mockSummary(u, p),
    setTelemetryProfile: async () => undefined,
    getTokenStatus: async () => false,
    getTokenRoles: async () => ({ adminSet: false, readSet: false }),
    setToken: async () => undefined,
    getSecurity: async () => null,
    getAp: async () => null,
    setApPassword: async () => ({}),
    setSecurity: async () => undefined,
    getBackupStatus: async () => null,
    createBackup: async () => ({ filesBackedUp: 0 }),
    restoreBackup: async () => ({ status: 'OK', filesRestored: 0, requiresReboot: true }),
    postOta: async () => ({ status: 'ok', message: '', partition: 'ota_0' }),
    getEspnow: async () => mockEspnow(),
    espnowAction: async () => ({ ok: true }),
    exportCsv: async () => null,
  };
}

const renderPage = (ui: React.ReactElement, transport: DeviceTransport = fakeTransport()) =>
  render(
    <ThemeProvider forcedTheme="dark">
      <DeviceProvider transport={transport}>
        <MemoryRouter>{ui}</MemoryRouter>
      </DeviceProvider>
    </ThemeProvider>,
  );

/** ConnectScreen is the pre-app gate — no DeviceProvider/router. */
const renderBare = (ui: React.ReactElement) =>
  render(<ThemeProvider forcedTheme="dark">{ui}</ThemeProvider>);

describe('page smoke tests', () => {

  it('ConfigurationPage renders the settings hub', async () => {
    renderPage(<ConfigurationPage />);
    expect(await screen.findByText('Настройки')).toBeInTheDocument();
  });

  it('EconomicsPage renders', async () => {
    renderPage(<EconomicsPage />);
    expect(await screen.findByText('Экономика')).toBeInTheDocument();
  });

  it('DevicesPage renders', async () => {
    renderPage(<DevicesPage />);
    expect(await screen.findByText('Точки')).toBeInTheDocument();
  });

  it('HistoryPage renders the heading', async () => {
    renderPage(<HistoryPage />);
    expect(await screen.findByText('История')).toBeInTheDocument();
  });

  it('EventsPage renders the heading', async () => {
    renderPage(<EventsPage />);
    expect(await screen.findByRole('heading', { name: 'События' })).toBeInTheDocument();
  });

  it('PointDetailPage mounts for a known unit', () => {
    const { container } = render(
      <ThemeProvider forcedTheme="dark">
        <DeviceProvider transport={fakeTransport()}>
          <MemoryRouter initialEntries={['/point/main']}>
            <Routes>
              <Route path="/point/:unitKey" element={<PointDetailPage />} />
            </Routes>
          </MemoryRouter>
        </DeviceProvider>
      </ThemeProvider>,
    );
    expect(container.firstChild).toBeTruthy();
  });

  // Pages behind a loading gate: assert they mount + produce DOM without crashing.
  it('DashboardPage mounts without crashing', () => {
    const { container } = renderPage(<DashboardPage />);
    expect(container.firstChild).toBeTruthy();
  });

  // The Overview has four distinct layouts. The scenario is a DEVICE setting (system.site_profile),
  // so each one is rendered from a device that reports it; Аналитика is a local view toggle.
  it.each([
    ['home', 'Сегодня по часам'],
    ['industrial', 'Линии нагрузки'],
    ['ev', 'Баланс нагрузки паркинга'],
  ])('DashboardPage renders the %s scenario from the device setting', async (profile, marker) => {
    window.localStorage.removeItem('rbgrid.site_profile');
    renderPage(<DashboardPage />, fakeTransport({ system: { site_profile: profile } }));
    expect(await screen.findByText(marker, {}, { timeout: 4000 })).toBeInTheDocument();
  });

  it('DashboardPage switches to the cross-cutting «Подробно» view', async () => {
    renderPage(<DashboardPage />);
    await screen.findByRole('heading', { name: 'Обзор' });
    fireEvent.click(screen.getByRole('button', { name: 'Подробно' }));
    await screen.findByText('Поток энергии');
  }, 20_000); // ~4.6 s alone; under a full parallel run it crossed jest's 5 s default

  it('ConfigurationPage offers the scenario choice (moved off the Overview header)', async () => {
    renderPage(<ConfigurationPage />);
    expect(await screen.findByText('Сценарий объекта')).toBeInTheDocument();
  });

  it('TariffsPage mounts without crashing', () => {
    const { container } = renderPage(<TariffsPage />);
    expect(container.firstChild).toBeTruthy();
  });

  it('OnboardingPage renders the wizard steps', async () => {
    renderPage(<OnboardingPage />);
    expect(await screen.findByText('Датчики')).toBeInTheDocument();
    expect(screen.getByText('Безопасность')).toBeInTheDocument();
  });

  it('ConnectScreen renders the brand', async () => {
    renderBare(<ConnectScreen />);
    expect(await screen.findByText('RBGrid')).toBeInTheDocument();
  });
});
