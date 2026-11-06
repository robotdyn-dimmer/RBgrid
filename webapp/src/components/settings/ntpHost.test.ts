import { ntpHostError, timeSaveBody } from './TimePanel';
import { numberRules } from '../../core/schema/fieldRules';

describe('time-sync limits (fw 1.1.6 save_config → 400, root time-ota-small-fixes)', () => {
  it('NTP host: non-empty, no spaces or «/», ≤ 63 chars; the backup may be empty', () => {
    expect(ntpHostError('pool.ntp.org', true)).toBeNull();
    expect(ntpHostError('', true)).not.toBeNull();
    expect(ntpHostError('', false)).toBeNull();
    expect(ntpHostError('pool ntp.org', true)).not.toBeNull();
    expect(ntpHostError('http://pool.ntp.org', true)).not.toBeNull();
    expect(ntpHostError('a'.repeat(63), true)).toBeNull();
    expect(ntpHostError('a'.repeat(64), true)).not.toBeNull();
  });

  it('numeric ranges for hubs without schema.limits', () => {
    expect(numberRules('resync_interval_sec')).toMatchObject({ min: 60, max: 86400, decimals: 0 });
    expect(numberRules('sntp_timeout_ms')).toMatchObject({ min: 1000, max: 60000 });
    expect(numberRules('sync_retry_count')).toMatchObject({ min: 1, max: 10 });
  });
});

describe('time save_config body (root v116-release-artifacts)', () => {
  const cfg = { timezone: 'UTC0', sntp_server: 'pool.ntp.org', sntp_server_backup: 'time.google.com', resync_interval_sec: 3600 };
  const draft = { ...cfg, timezone: 'CET-1CEST,M3.5.0,M10.5.0/3' };
  it('fw 1.1.6 merges: only the changed fields', () => {
    expect(timeSaveBody(draft, cfg, true)).toEqual({ timezone: 'CET-1CEST,M3.5.0,M10.5.0/3' });
  });
  it('fw ≤ 1.1.5 replaces /time.json: the whole config, so the NTP servers stay', () => {
    expect(timeSaveBody(draft, cfg, false)).toEqual(draft);
  });
});
