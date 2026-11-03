import { syncStatusWord } from './TimePanel';

test('time chip: every hub sync status gets a word', () => {
  expect(syncStatusWord('SYNCED')?.label).toBe('time.statusSynced');
  expect(syncStatusWord('SYNCING')?.label).toBe('time.statusSyncing');
  expect(syncStatusWord('INITIALIZING')?.label).toBe('time.statusSyncing');
  for (const s of ['UNINITIALIZED', 'TIMEOUT', 'ERROR', 'FALLBACK', 'SOMETHING_NEW']) {
    expect(syncStatusWord(s)).toEqual({ label: 'time.statusNotSynced', kind: 'error' });
  }
  expect(syncStatusWord(undefined)).toBeNull();
});
