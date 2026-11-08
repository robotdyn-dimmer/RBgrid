import React from 'react';
import { MemoryRouter } from 'react-router-dom';
import { render, screen, fireEvent, waitFor, within, act } from '@testing-library/react';
import { NodesPanel } from './NodesPanel';
import { ConfirmProvider } from '../common/ConfirmDialog';
import type { EspnowStatus } from '../../core/espnow/types';

let mockData: EspnowStatus | null = null;
const mockReload = jest.fn(async () => {});
const mockAction = jest.fn();
const mockJobs = { start: jest.fn(() => ({ id: 'j' })), resolve: jest.fn(), fail: jest.fn(), awaitReboot: jest.fn() };
const mockLoadDomain = jest.fn();
const mockSession = { transport: { espnowAction: (...a: unknown[]) => mockAction(...a) }, jobs: mockJobs, loadDomain: (...a: unknown[]) => mockLoadDomain(...a) };
jest.mock('../../core', () => ({
  ...jest.requireActual('../../core/espnow/types'),
  useEspnow: () => ({ data: mockData, loading: false, reloadNow: mockReload }),
  useDeviceSession: () => mockSession,
  useCapabilities: () => ({ ctModels: { 3: 'SCT-013-030, 30 A', 5: 'SCT-013-100, 100 A' } }),
}));
const mockHidden = { espnow: false };
jest.mock('../../constants/release', () => ({ get RELEASE_HIDDEN() { return mockHidden; } }));
jest.mock('../../hooks/useUnitsConfig', () => ({
  useUnitsConfig: () => ({ historyCount: 5, forKey: (id: string) => (id === '0x6B08' ? { name: 'Garage' } : undefined) }),
}));
jest.mock('../common/RebootNowButton', () => ({ RebootNowButton: () => <button type="button">Restart now</button> }));

const hub = (over: Partial<EspnowStatus> = {}): EspnowStatus => ({
  running: true, enabled: 'on', keys: true, max_nodes: 6, max_channels_per_node: 4, channel: 10,
  pairing_open: false, pairing_left_s: 0, paired_count: 0, beacons_sent: 1, rt_recv: 1, stats_recv: 1,
  nodes: [{
    mac: 'D8:13:2A:03:8F:A4', online: true, disabled: false, last_seen_s: 0,
    sync: { resid_sd_us: 315.1, skew_ppm: 6.99, fits: 103, loss_pct: 0 },
    channels: [{ ch: 8, ct_code: 3, unit_id: '0x6B08', present: true }],
  }],
  ...over,
});
const view = () => render(<MemoryRouter><ConfirmProvider><NodesPanel /></ConfirmProvider></MemoryRouter>);
const okDialog = async () => {
  const d = await screen.findByRole('dialog');
  const bs = within(d).getAllByRole('button');
  fireEvent.click(bs[bs.length - 1]);
};

describe('NodesPanel (fw 1.1.7, root espnow-nodes-ui)', () => {
  beforeEach(() => {
    mockAction.mockReset(); mockReload.mockImplementation(async () => {});
    mockJobs.start.mockImplementation(() => ({ id: 'j' }));
    mockLoadDomain.mockImplementation(async () => {});
    mockHidden.espnow = false;
  });

  it('keys:false → «not available in this firmware», nothing else', () => {
    mockData = hub({ keys: false });
    view();
    expect(screen.getByText(/not available in this hub firmware|недоступны в этой прошивке/)).toBeTruthy();
    expect(screen.queryByRole('button', { name: /Pair a node|Сопрячь узел/ })).toBeNull();
  });

  it('no keys field on a release build (hub before 1.1.7) → hidden; on a preview build → shown', () => {
    mockData = hub({ keys: undefined });
    mockHidden.espnow = true;
    const { container, unmount } = view();
    expect(container.textContent).toBe('');
    unmount();
    mockHidden.espnow = false;
    view();
    expect(screen.getByRole('button', { name: /Pair a node|Сопрячь узел/ })).toBeTruthy();
  });

  it('a node card: count, its channel links to the named point, history slots said before pairing', () => {
    mockData = hub();
    view();
    expect(screen.getByText(/1 of 6 nodes|узлов: 1 из 6/)).toBeTruthy();
    const link = screen.getByRole('link', { name: 'Garage' });
    expect(link.getAttribute('href')).toBe('/point/6B08');
    expect(screen.getByText(/5 of 12 points|5 из 12 точек/)).toBeTruthy();
    expect(screen.getByText(/channel 10|канал 10/)).toBeTruthy();
  });

  it('pair sends the chosen window; a 409 ESPNOW_DISABLED reads «turn the radio on»', async () => {
    mockData = hub();
    mockAction.mockResolvedValue({ ok: false, error: 'ESPNOW_DISABLED', message: 'x' });
    view();
    fireEvent.click(screen.getByRole('button', { name: /Pair a node|Сопрячь узел/ }));
    await waitFor(() => expect(mockAction).toHaveBeenCalledWith('pair', { seconds: 120 }));
    expect(await screen.findByText(/Turn it on first|Сначала включите/)).toBeTruthy();
  });

  it('an open window: countdown and cancel', async () => {
    mockData = hub({ pairing_open: true, pairing_left_s: 95 });
    mockAction.mockResolvedValue({ ok: true, pairing_open: false, paired: 0 });
    view();
    expect(screen.getByText(/1:35/)).toBeTruthy();
    fireEvent.click(screen.getByRole('button', { name: /Stop pairing|Прервать сопряжение/ }));
    await waitFor(() => expect(mockAction).toHaveBeenCalledWith('pair_cancel', {}));
  });

  it('paired_count > 0 → «paired, points already there», no restart (F2)', () => {
    mockData = hub({ paired_count: 1 });
    view();
    expect(screen.getByText(/Paired in this window: 1|Сопряжено за это окно: 1/)).toBeTruthy();
    expect(screen.queryByRole('button', { name: 'Restart now' })).toBeNull();
  });

  it('6 of 6 → pairing is off and says why', () => {
    const n = hub().nodes[0];
    mockData = hub({ nodes: Array.from({ length: 6 }, (_, i) => ({ ...n, mac: `00:00:00:00:00:0${i}` })) });
    view();
    expect((screen.getByRole('button', { name: /Pair a node|Сопрячь узел/ }) as HTMLButtonElement).disabled).toBe(true);
    expect(screen.getByText(/up to 6 nodes|до 6 узлов/)).toBeTruthy();
  });

  it('remove asks first (history goes too), then sends the mac', async () => {
    mockData = hub();
    mockAction.mockResolvedValue({ ok: true, removed_units: 1 });
    view();
    fireEvent.click(screen.getByRole('button', { name: /Delete|Удалить/ }));
    expect(await screen.findByText(/stored history|сохранённой историей/)).toBeTruthy();
    await okDialog();
    await waitFor(() => expect(mockAction).toHaveBeenCalledWith('remove', { mac: 'D8:13:2A:03:8F:A4' }));
    expect(await screen.findByText(/Node removed|Узел удалён/)).toBeTruthy();
  });

  it('radio off asks first when nodes exist, then hub_off', async () => {
    mockData = hub();
    mockAction.mockResolvedValue({ ok: true, running: false, enabled: false });
    view();
    fireEvent.click(screen.getByRole('checkbox'));
    await okDialog();
    await waitFor(() => expect(mockAction).toHaveBeenCalledWith('hub_off', {}));
  });

  it('planned fields show when a hub sends them', () => {
    mockData = hub();
    mockData.nodes[0] = { ...mockData.nodes[0], fw: '1.0.3', rssi_dbm: -61, config_state: 'pending' };
    view();
    expect(screen.getByText('1.0.3')).toBeTruthy();
    expect(screen.getByText('-61 dBm')).toBeTruthy();
    expect(screen.getByText(/settings not applied yet|ещё не применены/)).toBeTruthy();
  });

  it('a node not heard since the hub started (no sync, last_seen_s -1 or absent) renders (root espnow-panel-bench)', () => {
    const { sync: _s, last_seen_s: _l, ...bare } = hub().nodes[0];
    mockData = hub({ nodes: [{ ...bare, online: false, last_seen_s: -1 }, { ...bare, mac: 'AA:AA:AA:AA:AA:AA', online: false }] });
    view();
    expect(screen.getAllByText(/not heard since the hub started|нет сигнала с запуска хаба/)).toHaveLength(2);
    expect(screen.queryByText(/Lost packets|Потерянные пакеты/)).toBeNull();
    expect(screen.queryByText(/^receiving$|^данные идут$/)).toBeNull();
  });

  it('a section that throws shows a note instead of blanking the page', () => {
    const spy = jest.spyOn(console, 'error').mockImplementation(() => {});
    mockData = hub({ nodes: [null as never] });
    view();
    expect(screen.getByText(/couldn't be shown|не удалось показать/)).toBeTruthy();
    spy.mockRestore();
  });

  it('no «Settings» until the hub reports a node\'s config (F3/F5)', () => {
    mockData = hub();
    view();
    expect(screen.queryByRole('button', { name: /^Settings$|^Настройки$/ })).toBeNull();
  });

  it('Settings → CT per channel (the period is fixed, shown) → configure with channels only; all channels off is refused', async () => {
    const n = hub().nodes[0];
    mockData = hub({ nodes: [{ ...n, period_s: 900, config_state: 'applied', channels: [...(n.channels ?? []), { ch: 9, ct_code: 3, unit_id: '0x6B09', present: true }] }] });
    mockAction.mockResolvedValue({ ok: true, config_state: 'pending' });
    view();
    fireEvent.click(screen.getByRole('button', { name: /^Settings$|^Настройки$/ }));
    const d = await screen.findByRole('dialog');
    expect(within(d).getByText(/every 15 min, fixed|каждые 15 мин, фиксировано/)).toBeTruthy();
    const send = within(d).getByRole('button', { name: /Send to the node|Отправить узлу/ }) as HTMLButtonElement;
    expect(send.disabled).toBe(true); // nothing changed yet
    // both channels off → refused
    fireEvent.click(within(d).getByRole('checkbox', { name: /Channel 8|Канал 8/ }));
    fireEvent.click(within(d).getByRole('checkbox', { name: /Channel 9|Канал 9/ }));
    expect(within(d).getByText(/At least one channel|Хотя бы один канал/)).toBeTruthy();
    expect(send.disabled).toBe(true);
    fireEvent.click(within(d).getByRole('checkbox', { name: /Channel 8|Канал 8/ }));
    // channel 8 → the 100 A clamp
    fireEvent.mouseDown(within(d).getAllByText(/SCT-013-030/)[0]);
    fireEvent.click(await screen.findByRole('option', { name: /SCT-013-100/ }));
    fireEvent.click(send);
    await waitFor(() => expect(mockAction).toHaveBeenCalledWith('configure', {
      mac: 'D8:13:2A:03:8F:A4',
      config: { channels: [{ ch: 8, enabled: true, ct_code: 5 }, { ch: 9, enabled: false, ct_code: 3 }] },
    }));
    expect(await screen.findByText(/Sent\. The node applies|Отправлено\. Узел применит/)).toBeTruthy();
  });

  it('Turn off warns that the energy is discarded (no backfill, root Q3); Turn on asks nothing', async () => {
    mockData = hub();
    mockAction.mockResolvedValue({ ok: true, disabled: true });
    view();
    fireEvent.click(screen.getByRole('button', { name: /^Turn off$|^Выключить$/ }));
    expect(await screen.findByText(/won't be in History|не будет в «Истории»/)).toBeTruthy();
    await okDialog();
    await waitFor(() => expect(mockAction).toHaveBeenCalledWith('disable', { mac: 'D8:13:2A:03:8F:A4' }));
  });

  it('the dialog starts from the hub\'s config[] (ESPNOW_API aeacb18), not the channel list', async () => {
    const n = hub().nodes[0];
    mockData = hub({ nodes: [{ ...n, config_state: 'none', config: [{ ch: 8, enabled: true, ct_code: 5 }] }] });
    view();
    fireEvent.click(screen.getByRole('button', { name: /^Settings$|^Настройки$/ }));
    const d = await screen.findByRole('dialog');
    expect(within(d).getByText(/SCT-013-100/)).toBeTruthy();
    expect(within(d).queryByText(/SCT-013-030/)).toBeNull();
  });

  it('loss_pct absent in the first minute (fw e163afd) → «measuring», never NaN', () => {
    const n = hub().nodes[0];
    mockData = hub({ nodes: [{ ...n, sync: { resid_sd_us: 400, skew_ppm: 11, fits: 44 } }] });
    view();
    expect(screen.getByText(/measuring \(1 min\)|считается \(1 мин\)/)).toBeTruthy();
    expect(screen.queryByText(/NaN/)).toBeNull();
  });

  it('a node removed or paired re-reads units-config (slot count, names); a poll with no change doesn\'t', () => {
    mockData = hub();
    const { rerender } = view();
    const again = () => rerender(<MemoryRouter><ConfirmProvider><NodesPanel /></ConfirmProvider></MemoryRouter>);
    mockData = hub(); again();
    expect(mockLoadDomain).not.toHaveBeenCalled();
    mockData = hub({ nodes: [] }); again();
    expect(mockLoadDomain).toHaveBeenCalledWith('units');
  });

  it('a topology change reads units-config again after the read in flight, and once more 3 s later (B5)', async () => {
    jest.useFakeTimers();
    try {
      mockData = hub();
      const { rerender } = view();
      mockData = hub({ nodes: [] });
      rerender(<MemoryRouter><ConfirmProvider><NodesPanel /></ConfirmProvider></MemoryRouter>);
      await act(async () => {});
      expect(mockLoadDomain).toHaveBeenCalledTimes(2);
      act(() => { jest.advanceTimersByTime(3000); });
      expect(mockLoadDomain).toHaveBeenCalledTimes(3);
    } finally {
      jest.useRealTimers();
    }
  });

  it('a channel switched off in the node settings reads «off», not «no data»', () => {
    const n = hub().nodes[0];
    mockData = hub({ nodes: [{ ...n, config: [{ ch: 8, enabled: false, ct_code: 3 }] }] });
    view();
    expect(screen.getByText(/^off \(Settings\)$|^выключен \(Настройки\)$/)).toBeTruthy();
  });
});

