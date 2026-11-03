import React from 'react';
import { render, screen, waitFor, fireEvent } from '@testing-library/react';
import { AdminPanel } from './AdminPanel';
import { ConfirmProvider } from '../common/ConfirmDialog';

const mockApi = { getHubVersion: jest.fn(), getWifiStatus: jest.fn(), factoryReset: jest.fn(), rebootDevice: jest.fn() };
jest.mock('../../services/api', () => ({ getApiClient: () => mockApi, ApiRequestError: class extends Error {} }));

const view = () => render(<ConfirmProvider><AdminPanel /></ConfirmProvider>);

describe('AdminPanel factory reset (v1.1.2)', () => {
  beforeEach(() => Object.values(mockApi).forEach((f) => f.mockReset()));

  it('no reset button on a hub before 1.1.2', async () => {
    mockApi.getHubVersion.mockResolvedValue({ version: '1.1.1' });
    view();
    await waitFor(() => expect(mockApi.getHubVersion).toHaveBeenCalled());
    expect(screen.queryByRole('button', { name: /Factory reset|Заводской сброс/ })).toBeNull();
  });

  it('1.1.2: confirm with the history box → factory-reset, keep when unticked', async () => {
    mockApi.getHubVersion.mockResolvedValue({ version: '1.1.2' });
    mockApi.getWifiStatus.mockResolvedValue({ ap_ssid: 'rbgrid-7e01' });
    mockApi.factoryReset.mockResolvedValue({ status: 'ok' });
    view();
    const btn = await screen.findByRole('button', { name: /Factory reset|Заводской сброс/ });
    await waitFor(() => expect(mockApi.getWifiStatus).toHaveBeenCalled());
    await new Promise((r) => setTimeout(r, 0));
    fireEvent.click(btn);
    expect(await screen.findByText(/rbgrid-7e01/)).toBeTruthy();
    fireEvent.click(screen.getByRole('checkbox'));
    fireEvent.click(screen.getByRole('button', { name: /Reset the hub|Сбросить хаб/ }));
    await waitFor(() => expect(mockApi.factoryReset).toHaveBeenCalledWith('keep'));
    expect(await screen.findByText(/restarting as new|перезапускается как новый/)).toBeTruthy();
  });
});
