/**
 * TariffRibbon — the day's tariff zones with a «сейчас» marker (spec §4.4.2).
 * Reads the active plan via getTariffPlans() (same source as TariffsPage), lays its
 * TIME_RANGE zones onto a 24 h bar, highlights the current zone and marks the hour.
 * Net-metering/other zone types degrade to a flat bar; no plan → a muted empty state.
 */
import React, { useEffect, useState } from 'react';
import { Box, Stack, Typography } from '@mui/material';
import { Card } from '@rbgrid/design-system';
import { getApiClient } from '../../services/api';
import { TariffPlan, TariffZone } from '../../types/tariff-plan';
import { useDeviceClock } from '../../core/deviceTime';
import { useDsTokens } from '../../theme/useDsTokens';
import { dsFonts } from '../../theme/dsTokens';
import { useMoneyFormat } from '../../hooks/useMoneyFormat';
import { t } from '../../i18n';
import { zoneDisplayName } from '../../economics/tariffPlan';
import { grossOf } from '../../economics/costRate';

interface Seg { name: string; from: number; to: number; rate: number; active: boolean; }

/** Expand a plan's TIME_RANGE zones into hour-ordered [from,to) segments covering 0..24. */
function toSegments(plan: TariffPlan, isNow: (z: TariffZone) => boolean): Seg[] {
  const segs: Seg[] = [];
  for (const z of plan.zones ?? []) {
    const c = (z.conditions ?? []).find((x) => x.condition_type === 'TIME_RANGE');
    const p = c?.params as { start_hour?: number; end_hour?: number } | undefined;
    if (!p || p.start_hour == null || p.end_hour == null) continue;
    const name = zoneDisplayName(z.zone_id, z.zone_name) || t('cards.tariffZoneFallback');
    const rate = z.rate_buying_per_kwh ?? 0;
    if (p.start_hour <= p.end_hour) {
      segs.push({ name, from: p.start_hour, to: p.end_hour, rate, active: isNow(z) });
    } else {
      // wraps midnight → two pieces
      segs.push({ name, from: p.start_hour, to: 24, rate, active: isNow(z) });
      segs.push({ name, from: 0, to: p.end_hour, rate, active: isNow(z) });
    }
  }
  return segs.sort((a, b) => a.from - b.from);
}

/** `currentZone` = the hub's live zone (`system.tariff.zone`) — the config's
 *  is_currently_active was false on every hub before canon 2b3dc1fd (UX audit TAR-06). */
/** `gross`: the zone rates on the live rate's basis (+ levies, VAT) — the €/h beside it is (R02). */
export const TariffRibbon: React.FC<{ currentZone?: string | null; gross?: boolean }> = ({ currentZone, gross }) => {
  const clock = useDeviceClock();
  const ds = useDsTokens();
  const [plan, setPlan] = useState<TariffPlan | null>(null);
  const [loaded, setLoaded] = useState(false);
  const [failed, setFailed] = useState(false);

  // Read once; a failed read (no link at open) retries every 15 s instead of leaving the bar
  // empty for good (L3P-R07: «Tariff · today» stayed blank after the link came back).
  useEffect(() => {
    let on = true;
    let timer: ReturnType<typeof setTimeout> | undefined;
    const read = () => getApiClient().getTariffPlans()
      .then((env) => { if (on) { setPlan(env?.data?.list?.[0] ?? null); setFailed(false); setLoaded(true); } })
      .catch(() => { if (on) { setFailed(true); timer = setTimeout(read, 15_000); } });
    read();
    return () => { on = false; if (timer) clearTimeout(timer); };
  }, []);

  const isNow = (z: TariffZone) => (currentZone ? z.zone_id === currentZone : !!z.is_currently_active);
  const g = gross ? grossOf(plan) : null;
  const segs = (plan ? toSegments(plan, isNow) : []).map((x) => (g ? { ...x, rate: g(x.rate) } : x));
  const now = clock.parts(Math.floor(Date.now() / 1000));
  const nowFrac = (now.h + now.mi / 60) / 24;
  // Currency/precision from the device (plan's ISO code; schema decimals) — never hardcoded.
  const mf = useMoneyFormat(plan?.currency_code);

  return (
    <Card>
      <Stack spacing={1.25} sx={{ py: 0.5 }}>
        <Typography sx={{ fontWeight: 700, fontSize: 15 }}>{t('cards.tariffTitle')}</Typography>

        {failed && !plan ? (
          <Typography sx={{ color: ds.inkMuted, py: 1 }}>{t('cards.tariffNoLink')}</Typography>
        ) : loaded && segs.length === 0 ? (
          <Typography sx={{ color: ds.inkMuted, py: 1 }}>{t('cards.tariffNoZones')}</Typography>
        ) : (
          <>
            <Box sx={{ position: 'relative', height: 30, borderRadius: 1, overflow: 'hidden', bgcolor: ds.surfaceRaised }}>
              {segs.map((s, i) => (
                <Box
                  key={i}
                  title={`${s.name}: ${mf.rate(s.rate)} · ${s.from}:00–${s.to}:00`}
                  sx={{
                    position: 'absolute', top: 0, bottom: 0,
                    left: `${(s.from / 24) * 100}%`, width: `${((s.to - s.from) / 24) * 100}%`,
                    // Inactive zones alternate two tones so neighbours stay apart (INP-25).
                    bgcolor: s.active ? ds.primary : i % 2 ? ds.borderStrong : ds.border,
                    borderRight: `2px solid ${ds.surface}`,
                  }}
                />
              ))}
              {/* «сейчас» marker */}
              <Box sx={{ position: 'absolute', top: -2, bottom: -2, left: `${nowFrac * 100}%`, width: '2px', bgcolor: ds.ink }} />
            </Box>
            <Stack direction="row" justifyContent="space-between">
              <Typography sx={{ fontSize: 11, color: ds.inkMuted, fontFamily: dsFonts.mono }}>0:00</Typography>
              <Typography sx={{ fontSize: 11, color: ds.inkMuted, fontFamily: dsFonts.mono }}>12:00</Typography>
              <Typography sx={{ fontSize: 11, color: ds.inkMuted, fontFamily: dsFonts.mono }}>24:00</Typography>
            </Stack>
            <Stack direction="row" spacing={2} flexWrap="wrap" useFlexGap>
              {dedupeZones(segs).map((s, i) => (
                <Stack key={i} direction="row" spacing={0.75} alignItems="center">
                  <Box sx={{ width: 10, height: 10, borderRadius: 0.5, bgcolor: s.active ? ds.primary : ds.borderStrong }} />
                  <Typography sx={{ fontSize: 12, color: s.active ? ds.ink : ds.inkMuted }}>
                    {s.name} · {mf.rate(s.rate)}
                  </Typography>
                </Stack>
              ))}
            </Stack>
          </>
        )}
      </Stack>
    </Card>
  );
};

function dedupeZones(segs: Seg[]): Seg[] {
  const seen = new Set<string>();
  const out: Seg[] = [];
  for (const s of segs) {
    if (seen.has(s.name)) continue;
    seen.add(s.name);
    out.push(s);
  }
  return out;
}
