/**
 * Regression (review 02 B-21): Настройки must render the config domains once their GETs land,
 * with the REAL DeviceProvider + DirectTransport (demo). Before the session-version context a
 * notify() never reached the editors and the page sat on spinners forever.
 */
import React from 'react';
import { render, screen, waitFor } from '@testing-library/react';
import { MemoryRouter } from 'react-router-dom';
import { ThemeProvider } from '../theme/ThemeProvider';
import { DeviceProvider } from '../core';
import { connectDemo, disconnect } from '../services/connection';
import { ConfigurationPage } from './ConfigurationPage';

beforeAll(() => connectDemo());
afterAll(() => disconnect());

it('loads and renders every config domain (no endless spinners)', async () => {
  render(
    <ThemeProvider forcedTheme="dark">
      <DeviceProvider><MemoryRouter><ConfigurationPage /></MemoryRouter></DeviceProvider>
    </ThemeProvider>,
  );
  await waitFor(() => expect(document.querySelectorAll('.MuiCircularProgress-root').length).toBe(0));
  expect(screen.getAllByText('Название объекта').length).toBeGreaterThan(0);
  expect(screen.queryByText(/Нет данных с устройства/)).toBeNull();
});
