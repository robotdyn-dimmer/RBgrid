import { renderHook, act, waitFor } from '@testing-library/react';
import { useHistory } from './useHistory';
import { getApiClient } from '../services/api';
import { connectDevice } from '../services/connection';

jest.mock('../core/deviceTime', () => {
  const clock = { dayStart: (ts: number) => ts - (ts % 86400) };
  return { ...jest.requireActual('../core/deviceTime'), useDeviceClock: () => clock };
});

/** Kimi #4: switching point A → B while A's answer is still on the way must keep B's series. */
describe('useHistory — only the latest request writes', () => {
  beforeAll(() => connectDevice('192.168.1.139'));

  it('a late answer for the previous point is dropped', async () => {
    const pending: Record<string, (v: unknown) => void> = {};
    jest.spyOn(getApiClient(), 'getHistory').mockImplementation(((q: { unit_id: string }) =>
      new Promise((res) => { pending[q.unit_id] = res; })) as never);
    const { result, rerender } = renderHook(({ id }) => useHistory(id, 'hour', 'energy'), { initialProps: { id: 'A' } });
    rerender({ id: 'B' });
    const rec = (id: string) => ({ data: [{ ts: 1_760_000_000, e_in: id === 'A' ? 1 : 2 }], meta: { has_more: false } });
    await act(async () => { pending.B(rec('B')); });
    await waitFor(() => expect(result.current.records).toHaveLength(1));
    await act(async () => { pending.A(rec('A')); });
    expect((result.current.records[0] as unknown as { e_in: number }).e_in).toBe(2);
    expect(result.current.loading).toBe(false);
  });
});
