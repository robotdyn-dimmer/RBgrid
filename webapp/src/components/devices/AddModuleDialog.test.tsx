import React from 'react';
import { render, screen, waitFor, fireEvent } from '@testing-library/react';
import { AddModuleDialog } from './AddModuleDialog';

const mockApi = { getI2c: jest.fn(), setI2cModuleAddr: jest.fn(), scanI2c: jest.fn(), addI2cModule: jest.fn() };
jest.mock('../../services/api', () => ({ getApiClient: () => mockApi, ApiRequestError: class extends Error {} }));

const m = (module: number, addr: number) => ({ module, bus: 0, addr, present: true, model: 'STD-UI7' });
const st = (mods: ReturnType<typeof m>[]) => ({ buses: [{ bus: 0, up: true }], modules: mods, max_modules: 3 });

describe('AddModuleDialog', () => {
  beforeEach(() => Object.values(mockApi).forEach((f) => f.mockReset()));

  it('moves the module at 0x50, then scans and adds the new one', async () => {
    mockApi.getI2c.mockResolvedValue(st([m(0, 0x50)]));
    mockApi.setI2cModuleAddr.mockResolvedValue({ status: 'ok', ...st([m(0, 0x51)]) });
    mockApi.scanI2c.mockResolvedValue({ status: 'ok', devices: [{ addr: 0x50, module: null, kind: 'rbamp', fw: 'STD-1.7.2' }, { addr: 0x51, module: 0, kind: 'rbamp' }] });
    mockApi.addI2cModule.mockResolvedValue({ status: 'ok', ...st([m(0, 0x51), m(1, 0x50)]) });
    const changed = jest.fn();
    render(<AddModuleDialog open onClose={() => undefined} onChanged={changed} />);
    fireEvent.click(await screen.findByRole('button', { name: /0x51/ }));
    await waitFor(() => expect(mockApi.setI2cModuleAddr).toHaveBeenCalledWith(0, 0x51));
    fireEvent.click(await screen.findByRole('button', { name: /^(addmod\.scan|Scan|Найти)$/ }));
    await waitFor(() => expect(mockApi.scanI2c).toHaveBeenCalledWith(0));
    fireEvent.click(await screen.findByRole('button', { name: /^(addmod\.add|Add|Добавить)$/ }));
    await waitFor(() => expect(mockApi.addI2cModule).toHaveBeenCalledWith(0, 0x50));
    // a third slot is left: «add another» brings back the move step for the new module at 0x50
    fireEvent.click(await screen.findByRole('button', { name: /another|ещё/i }));
    expect(changed).toHaveBeenCalledTimes(2);
    expect(await screen.findByRole('button', { name: /0x52/ })).toBeTruthy();
  });

  it('a stuck bus says so', async () => {
    mockApi.getI2c.mockResolvedValue(st([m(0, 0x51)]));
    mockApi.scanI2c.mockResolvedValue({ status: 'error', code: 'BUS_STUCK', message: 'every address answers' });
    render(<AddModuleDialog open onClose={() => undefined} onChanged={() => undefined} />);
    fireEvent.click(await screen.findByRole('button', { name: /^(addmod\.scan|Scan|Найти)$/ }));
    expect(await screen.findByText(/SDA/)).toBeTruthy();
  });

  it('a module already at another address is added without freeing 0x50 (root 03.11)', async () => {
    mockApi.getI2c.mockResolvedValue(st([m(0, 0x50)]));
    mockApi.scanI2c.mockResolvedValue({ status: 'ok', devices: [{ addr: 0x50, module: 0, kind: 'rbamp' }, { addr: 0x52, module: null, kind: 'rbamp', model: 'BASIC-UI3', fw: '1.4.20' }] });
    mockApi.addI2cModule.mockResolvedValue({ status: 'ok', ...st([m(0, 0x50), m(1, 0x52)]) });
    render(<AddModuleDialog open onClose={() => undefined} onChanged={() => undefined} />);
    fireEvent.click(await screen.findByRole('button', { name: /another address|другом адресе/ }));
    expect(await screen.findByText(/BASIC-UI3/)).toBeTruthy();
    fireEvent.click(await screen.findByRole('button', { name: /^(addmod\.add|Add|Добавить)$/ }));
    await waitFor(() => expect(mockApi.addI2cModule).toHaveBeenCalledWith(0, 0x52));
    expect(mockApi.setI2cModuleAddr).not.toHaveBeenCalled();
  });
});
