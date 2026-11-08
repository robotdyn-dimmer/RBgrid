import { getApiClient } from '../services/api';
import { connectDevice } from '../services/connection';
import { useHardware } from './useHardware';
import { renderHook, act, waitFor } from '@testing-library/react';

/** Kimi #15: hub A's sensor list answering after a switch to hub B is not shown as B's. */
it('a late answer from the previous hub is dropped', async () => {
  const pending: Array<(v: unknown) => void> = [];
  jest.spyOn(getApiClient(), 'getSensors').mockImplementation((() => new Promise((res) => { pending.push(res); })) as never);
  connectDevice('192.168.1.10');
  const { result } = renderHook(() => useHardware());
  expect(pending).toHaveLength(1);
  act(() => connectDevice('192.168.1.20'));
  const { result: r2 } = renderHook(() => useHardware());
  expect(pending).toHaveLength(2); // B's read isn't held back by A's
  const list = (id: string) => ({ data: { sensors: [{ sensor_id: id }], modules: [] } });
  await act(async () => { pending[1](list('B')); });
  await waitFor(() => expect(r2.current.sensors).toHaveLength(1));
  await act(async () => { pending[0](list('A')); });
  expect((r2.current.sensors[0] as unknown as { sensor_id: string }).sensor_id).toBe('B');
  expect(result.current.sensors).toBeDefined();
});
