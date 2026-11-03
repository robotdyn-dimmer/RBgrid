/**
 * useTariffPlan — the device's ACTIVE tariff plan (GET tariff-plan-config), shared by the
 * tariff ribbon, the savings card and the money lens. Fetched once per mount; null when the
 * device has none / OSS / offline.
 */
import { useEffect, useState } from 'react';
import { getApiClient } from '../services/api';
import { TariffPlan } from '../types/tariff-plan';

export function useTariffPlan(): { plan: TariffPlan | null; loaded: boolean } {
  const [plan, setPlan] = useState<TariffPlan | null>(null);
  const [loaded, setLoaded] = useState(false);
  useEffect(() => {
    let on = true;
    getApiClient().getTariffPlans()
      .then((env) => {
        if (!on) return;
        const list = env?.data?.list ?? [];
        setPlan(list.find((p) => p.is_active) ?? list[0] ?? null);
      })
      .catch(() => { if (on) setPlan(null); })
      .finally(() => { if (on) setLoaded(true); });
    return () => { on = false; };
  }, []);
  return { plan, loaded };
}
