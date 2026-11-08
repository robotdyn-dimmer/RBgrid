import React from 'react';
import { render, screen, fireEvent, waitFor } from '@testing-library/react';
import { BackupPanel } from './BackupPanel';
import { ConfirmProvider } from '../common/ConfirmDialog';

const mockTransport = { getBackupStatus: jest.fn(), createBackup: jest.fn(), restoreBackup: jest.fn() };
const mockApi = { rebootDevice: jest.fn() };
const mockSession = { transport: mockTransport };
jest.mock('../../core', () => ({ useDeviceSession: () => mockSession }));
jest.mock('../../services/api', () => ({ getApiClient: () => mockApi }));

const httpErr = (status: number, data: unknown = {}) => Object.assign(new Error(`HTTP ${status}`), { response: { status, data } });
const view = () => render(<ConfirmProvider><BackupPanel /></ConfirmProvider>);

const restoreWith = async (err: Error) => {
  mockTransport.restoreBackup.mockRejectedValue(err);
  view();
  const btn = await screen.findByRole('button', { name: /Restore and restart|Восстановить и перезагрузить/ });
  await waitFor(() => expect((btn as HTMLButtonElement).disabled).toBe(false));
  fireEvent.click(btn);
  const dialog = await screen.findByRole('dialog');
  const bs = dialog.querySelectorAll('button');
  fireEvent.click(bs[bs.length - 1]);
};

describe('BackupPanel error answers (fw 1.1.7, root restore-point-errors)', () => {
  beforeEach(() => {
    [...Object.values(mockTransport), ...Object.values(mockApi)].forEach((f) => f.mockReset());
    mockTransport.getBackupStatus.mockResolvedValue({ exists: true, files: 6 });
  });

  it('save 500 → «couldn\'t save», and the status is read again', async () => {
    mockTransport.getBackupStatus.mockResolvedValue({ exists: false, files: 0 });
    mockTransport.createBackup.mockRejectedValue(httpErr(500, { status: 'ERROR', files_backed_up: 0 }));
    view();
    const btn = await screen.findByRole('button', { name: /Save restore point|Сохранить точку/ });
    await waitFor(() => expect((btn as HTMLButtonElement).disabled).toBe(false));
    fireEvent.click(btn);
    expect(await screen.findByText(/Couldn't save the restore point|Не удалось сохранить точку/)).toBeTruthy();
    expect(mockTransport.getBackupStatus).toHaveBeenCalledTimes(2);
  });

  it('restore 409 BACKUP_INCOMPLETE → «incomplete», no reboot', async () => {
    await restoreWith(httpErr(409, { status: 'ERROR', error_code: 'BACKUP_INCOMPLETE', message: 'x' }));
    expect(await screen.findByText(/is incomplete|неполная/)).toBeTruthy();
    expect(mockApi.rebootDevice).not.toHaveBeenCalled();
  });

  it('restore 500 RESTORE_FAILED → «put back the settings», no reboot', async () => {
    await restoreWith(httpErr(500, { status: 'ERROR', error_code: 'RESTORE_FAILED', message: 'x' }));
    expect(await screen.findByText(/put back the settings|вернул настройки/)).toBeTruthy();
    expect(mockApi.rebootDevice).not.toHaveBeenCalled();
  });

  it('restore 404 → «no restore point»', async () => {
    await restoreWith(httpErr(404));
    expect(await screen.findByText(/No restore point|Точки восстановления нет/)).toBeTruthy();
  });

  it('sealed:false (fw 1.1.7) → «incomplete» before any click, Restore off; Save stays on', async () => {
    mockTransport.getBackupStatus.mockResolvedValue({ exists: true, files: 6, sealed: false });
    view();
    expect(await screen.findByText(/is incomplete|неполная/)).toBeTruthy();
    const restore = screen.getByRole('button', { name: /Restore and restart|Восстановить и перезагрузить/ }) as HTMLButtonElement;
    const save = screen.getByRole('button', { name: /Save restore point|Сохранить точку/ }) as HTMLButtonElement;
    expect(restore.disabled).toBe(true);
    expect(save.disabled).toBe(false);
  });

  it('no seal (older hub or point) → usable as before', async () => {
    mockTransport.getBackupStatus.mockResolvedValue({ exists: true, files: 6 });
    view();
    const restore = await screen.findByRole('button', { name: /Restore and restart|Восстановить и перезагрузить/ }) as HTMLButtonElement;
    await waitFor(() => expect(restore.disabled).toBe(false));
    expect(screen.queryByText(/is incomplete|неполная/)).toBeNull();
  });
});
