import { renderHook } from '@testing-library/react';
import { useQuietRefresh, HISTORY_REFRESH_MS } from './useHistory';

describe('charts refresh quietly (operator 20.10: no auto-update)', () => {
  beforeEach(() => jest.useFakeTimers());
  afterEach(() => jest.useRealTimers());

  it('re-reads hours about once a minute, quietly', () => {
    const fn = jest.fn();
    renderHook(() => useQuietRefresh(fn, true, HISTORY_REFRESH_MS.hour));
    jest.advanceTimersByTime(HISTORY_REFRESH_MS.hour + 5000);
    expect(fn).toHaveBeenCalledWith(true);
    expect(fn).toHaveBeenCalledTimes(1);
  });
  it('does nothing when disabled (demo / no point)', () => {
    const fn = jest.fn();
    renderHook(() => useQuietRefresh(fn, false));
    jest.advanceTimersByTime(10 * 60_000);
    expect(fn).not.toHaveBeenCalled();
  });
});
