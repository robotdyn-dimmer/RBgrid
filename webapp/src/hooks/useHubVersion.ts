import { useEffect, useState } from 'react';
import { getApiClient } from '../services/api';

/** The hub's firmware version string, null while unknown or when the hub doesn't say. */
export function useHubVersion(): string | null {
  const [v, setV] = useState<string | null>(null);
  useEffect(() => {
    let alive = true;
    getApiClient().getHubVersion().then((r) => { if (alive) setV(r.version ?? null); }).catch(() => undefined);
    return () => { alive = false; };
  }, []);
  return v;
}
