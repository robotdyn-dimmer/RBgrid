/**
 * TariffsPage — edit the active tariff plan: header fields + zones (rate/conditions) + a
 * taxes/charges summary. Loads the full BaseResponse and POSTs it back mutated (like units-config).
 * v1 edits the common fields; progressive/fixed-payments shown as summary. The hub applies a
 * saved plan LIVE (canon 4988e0c7) — no reboot hint; the page re-reads the plan after a save.
 * Zones carry rate_selling_per_kwh + net_metering_ratio. Only the sell rate is shown: the hub settles
 * net metering by the plan-wide ratio and nothing reads the zone's (root 02.11 / 03.11) — the POST
 * echoes it unchanged.
 * Renders through the @rbgrid/design-system surface (Card / FormField / ToggleRow / StatusChip / InfoField).
 */
import React, { useEffect, useState } from 'react';
import { HelpTip } from '../components/common/HelpTip';
import { Box, Typography, Button, Stack, Alert, CircularProgress, Grid, IconButton, ToggleButton, ToggleButtonGroup } from '@mui/material';
import AddIcon from '@mui/icons-material/Add';
import DeleteOutlineIcon from '@mui/icons-material/DeleteOutline';
import { zoneCoverage, zoneRange, zoneDays, daysText, ALL_DAYS, hhmm, parseHhmm, WeekSpan } from '../economics/zoneCoverage';
import { useDsTokens } from '../theme/useDsTokens';
import { useConfirm } from '../components/common/ConfirmDialog';
import { TimeField } from '../components/common/TimeField';
import { Card, FormField, ToggleRow, StatusChip, InfoField } from '@rbgrid/design-system';
import { getApiClient } from '../services/api';
import { isDemo } from '../services/connection';
import { BaseResponse } from '../types/api';
import { TariffPlanConfigData, TariffPlan, TariffZone, TariffZoneCondition } from '../types/tariff-plan';
import {
  TARIFF_PLAN_TYPE_NAMES, TARIFF_ZONE_ID_NAMES, TariffPlanType, TariffZoneId,
} from '../constants/enums';
import { t, tCode, intlLocale, dateLocale } from '../i18n';
import { useUnsavedChanges } from '../app/unsavedGuard';
import { DecimalField } from '../components/common/DecimalField';
import { zoneDisplayName } from '../economics/tariffPlan';
import { toSingleZone } from '../components/onboarding/quickTariffPlan';
import { useCapabilities } from '../core';

/** Zone types the editor offers. HOLIDAY isn't: the v1.1.x hub has no holiday dates to fill
 *  (root 02.11 — the zone never activates); a plan that already has one still shows it. */
const ZONE_TYPES_OFFERED: TariffZoneId[] = TARIFF_ZONE_ID_NAMES.filter((z) => z !== 'HOLIDAY');
/** Fixed payments: stored by the hub, counted in no cost yet (v1.1.x). */
const SHOW_FIXED_PAYMENTS = false;

const pad = (n: number) => String(n ?? 0).padStart(2, '0');
/** ISO 4217 codes offered in the currency picker (the plan's own code is always kept). */
const CURRENCIES = ['RUB', 'EUR', 'USD', 'GBP', 'UAH', 'KZT', 'BYN', 'PLN', 'CZK', 'CHF', 'TRY', 'CNY', 'INR', 'JPY'];

function condLabel(c: TariffZoneCondition): string {
  if (c.condition_type === 'TIME_RANGE') {
    const p = c.params as Record<string, number>;
    return `${p.start_hour}:${pad(p.start_minute)}–${p.end_hour}:${pad(p.end_minute)}`;
  }
  return c.condition_type;
}

export const TariffsPage: React.FC = () => {
  const [env, setEnv] = useState<BaseResponse<TariffPlanConfigData> | null>(null);
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [msg, setMsg] = useState<{ sev: 'success' | 'error'; text: string } | null>(null);

  // The plan as loaded — Save is enabled only when the edit differs from it (UX audit HEU-04).
  const [orig, setOrig] = useState<string>('');
  // Inline field errors by field id; Save is blocked while any stands (TAR-05).
  const [errors, setErrors] = useState<Record<string, string | null>>({});
  const setErr = (id: string, e: string | null) => setErrors((m) => (m[id] === e ? m : { ...m, [id]: e }));
  const caps = useCapabilities();
  const rateDecimals = caps.money.rateDecimals;

  // The zone in force NOW, from the hub's live tariff (UX audit TAR-06: the config flag was
  // false everywhere; the demo pinned «День» even at night).
  const [liveZone, setLiveZone] = useState<string | null>(null);
  useEffect(() => {
    let on = true;
    getApiClient().getLatestTelemetry()
      .then((f) => { const z = (f as { system?: { tariff?: { zone?: string; buy_rate?: number } } }).system?.tariff; if (on && z?.zone && (z.buy_rate ?? 0) > 0) setLiveZone(z.zone); })
      .catch(() => undefined);
    return () => { on = false; };
  }, []);
  const [reloadKey, setReloadKey] = useState(0);
  useEffect(() => {
    let on = true;
    getApiClient().getTariffPlans()
      .then((e) => { if (on) { setEnv(e); setOrig(JSON.stringify(e)); setErrors({}); } })
      .catch(() => { if (on) setMsg({ sev: 'error', text: t('tariffs.loadFailed') }); })
      .finally(() => { if (on) setLoading(false); });
    return () => { on = false; };
  }, [reloadKey]);

  const plan: TariffPlan | null = env?.data?.list?.[0] ?? null;

  const updatePlan = (patch: Partial<TariffPlan>) =>
    setEnv((e) => {
      if (!e) return e;
      const list = [...e.data.list];
      list[0] = { ...list[0], ...patch };
      return { ...e, data: { ...e.data, list } };
    });

  const updateZone = (i: number, patch: Partial<TariffZone>) =>
    setEnv((e) => {
      if (!e) return e;
      const list = [...e.data.list];
      const zones = [...list[0].zones];
      zones[i] = { ...zones[i], ...patch };
      list[0] = { ...list[0], zones };
      return { ...e, data: { ...e.data, list } };
    });

  // ── zone editing (W2-3: TAR-03 — time was a caption; no add/remove; no 24-h check) ──
  const confirm = useConfirm();
  const setZoneTime = (i: number, which: 'start' | 'end', value: string) => {
    const min = parseHhmm(value);
    if (min == null) return;
    setEnv((e) => {
      if (!e) return e;
      const list = [...e.data.list];
      const zones = [...list[0].zones];
      const z = zones[i];
      const conds = [...(z.conditions ?? [])];
      let ci = conds.findIndex((c) => c.condition_type === 'TIME_RANGE');
      if (ci < 0) { conds.push({ condition_type: 'TIME_RANGE', is_active: true, priority: 10, params: { start_hour: 0, start_minute: 0, end_hour: 0, end_minute: 0, spans_midnight: false } }); ci = conds.length - 1; }
      const p = { ...(conds[ci].params as Record<string, unknown>) };
      if (which === 'start') { p.start_hour = Math.floor(min / 60) % 24; p.start_minute = min % 60; }
      else { p.end_hour = Math.floor(min / 60) % 24; p.end_minute = min % 60; }
      const s = (p.start_hour as number) * 60 + (p.start_minute as number);
      const en = (p.end_hour as number) * 60 + (p.end_minute as number);
      p.spans_midnight = en <= s;
      conds[ci] = { ...conds[ci], params: p };
      zones[i] = { ...z, conditions: conds };
      list[0] = { ...list[0], zones };
      return { ...e, data: { ...e.data, list } };
    });
  };
  const addZone = () => setEnv((e) => {
    if (!e) return e;
    const list = [...e.data.list];
    const used = new Set(list[0].zones.map((z) => String(z.zone_id)));
    const id = ZONE_TYPES_OFFERED.find((x) => !used.has(x));
    if (!id) return e;
    // A new zone takes the first FREE hours of the week (TAR-R01: 00:00–00:00 = all day, and by its
    // type's priority it overrode the whole plan); with no free hours it starts switched off
    // until its hours are set. The selling price goes explicitly — without one the hub derives
    // buy × ratio (root: 0,315 next to 0,12 on the other zones, TAR-R05).
    const cov = zoneCoverage(list[0].zones, list[0].default_zone_id);
    const gap = cov.gaps[0];
    const tr = (a: number, b: number) => ({ start_hour: Math.floor(a / 60) % 24, start_minute: a % 60, end_hour: Math.floor(b / 60) % 24, end_minute: b % 60, spans_midnight: b % 1440 <= a });
    const sell = list[0].zones.find((z) => z.rate_selling_per_kwh != null)?.rate_selling_per_kwh ?? list[0].default_selling_rate_per_kwh;
    const zones = [...list[0].zones, {
      zone_id: id, zone_name: tCode('zone', id), rate_buying_per_kwh: list[0].zones[0]?.rate_buying_per_kwh ?? 0, is_enabled: !!gap,
      // Never left out: an unsent price is derived (net metering on) or kept from the slot's old zone (off).
      rate_selling_per_kwh: sell ?? 0,
      conditions: [
        { condition_type: 'TIME_RANGE' as const, is_active: true, priority: 10, params: gap ? tr(gap.start, gap.end) : tr(0, 60) },
        ...(gap && gap.days !== ALL_DAYS ? [{ condition_type: 'WEEKDAY' as const, is_active: true, priority: 5, params: { weekday_mask: gap.days } }] : []),
      ],
    } as TariffZone];
    list[0] = { ...list[0], zones, active_zones_count: zones.length };
    return { ...e, data: { ...e.data, list } };
  });
  // Days of a zone (W2-3): Monday-origin WEEKDAY mask — the one day condition the hub parses on
  // a save. A zone that never had one gets none for «every day»; a zone that had one keeps it
  // with all 7 bits — hubs before canon 54fffc83 kept listing a removed condition from its old
  // slot, so «removed» would read back as the old days. WEEKEND is replaced by its mask.
  const setZoneDays = (i: number, mask: number) => setEnv((e) => {
    if (!e) return e;
    const list = [...e.data.list];
    const zones = [...list[0].zones];
    const z = zones[i];
    const hadDays = (z.conditions ?? []).some((c) => c.condition_type === 'WEEKDAY' || c.condition_type === 'WEEKEND');
    const conds = (z.conditions ?? []).filter((c) => c.condition_type !== 'WEEKDAY' && c.condition_type !== 'WEEKEND');
    if (mask !== ALL_DAYS || hadDays) conds.push({ condition_type: 'WEEKDAY', is_active: true, priority: 5, params: { weekday_mask: mask } });
    zones[i] = { ...z, conditions: conds };
    list[0] = { ...list[0], zones };
    return { ...e, data: { ...e.data, list } };
  });
  const removeZone = async (i: number) => {
    const p0 = env?.data?.list?.[0];
    const z = p0?.zones[i];
    if (!z) return;
    // Where the removed zone's hours go: another zone, else the plan's default zone (REVIEW-pass2).
    const def = p0?.default_zone_id != null ? p0.zones.find((x) => String(x.zone_id) === String(p0.default_zone_id)) : undefined;
    const body = !def ? t('tariffs.removeZoneBody')
      : def === z ? t('tariffs.removeZoneBodyDefault')
        : t('tariffs.removeZoneBodyToDefault', { zone: zoneDisplayName(def.zone_id, def.zone_name) });
    if (!(await confirm({ title: t('tariffs.removeZoneTitle', { name: zoneDisplayName(z.zone_id, z.zone_name) }), body, confirmLabel: t('common.delete'), danger: true }))) return;
    setEnv((e) => {
      if (!e) return e;
      const list = [...e.data.list];
      const zones = list[0].zones.filter((_, j) => j !== i);
      list[0] = { ...list[0], zones, active_zones_count: zones.length };
      return { ...e, data: { ...e.data, list } };
    });
  };
  // The week as the hub will price it: overlaps are resolved by zone type (not an error); hours
  // with no zone fall to the default zone — an error only when there is none.
  const planZones = env?.data?.list?.[0]?.zones ?? [];
  const coverage = zoneCoverage(planZones, env?.data?.list?.[0]?.default_zone_id);
  const noDays = planZones.some((z) => z.is_enabled && zoneDays(z) === 0);
  const coverageBad = (coverage.gaps.length > 0 && coverage.fallback === null) || noDays;
  const ds = useDsTokens();
  const dayShort = t('tariffs.days').split(',');
  const zoneColor = (i: number) => ds.series[i % ds.series.length];
  const spanText = (g: WeekSpan) => `${g.days === ALL_DAYS ? '' : `${daysText(g.days, dayShort, t('tariffs.everyDay'))} `}${hhmm(g.start)}–${hhmm(g.end)}`;
  const zName = (i: number) => { const z = planZones[i]; return z ? zoneDisplayName(z.zone_id, z.zone_name) : '—'; };
  // One row when every day is priced alike, else Mon..Sun.
  const sameWeek = coverage.week.every((row) => row.every((v, m) => v === coverage.week[0][m]));
  const weekRows = sameWeek ? [{ label: t('tariffs.weekSame'), row: coverage.week[0] }] : coverage.week.map((row, d) => ({ label: dayShort[d], row }));
  const runsOf = (row: Int16Array) => {
    const out: { a: number; b: number; z: number }[] = [];
    for (let m = 0; m < 1440; m++) { const last = out[out.length - 1]; if (last && last.z === row[m]) last.b = m + 1; else out.push({ a: m, b: m + 1, z: row[m] }); }
    return out;
  };

  const dirty = !!env && JSON.stringify(env) !== orig;
  useUnsavedChanges('tariffs', dirty);
  const nameMissing = !!env && !(env.data?.list?.[0]?.plan_name ?? '').trim();
  const hasErrors = nameMissing || coverageBad || Object.values(errors).some(Boolean);

  const save = async () => {
    if (!env || hasErrors) return;
    setSaving(true);
    setMsg(null);
    try {
      await getApiClient().saveTariffPlans(env);
      setMsg({ sev: 'success', text: isDemo() ? t('tariffs.savedDemo') : t('tariffs.saved') });
      setReloadKey((k) => k + 1); // applied live → show the hub's view (active zone, derived sell)
    } catch {
      setMsg({ sev: 'error', text: t('tariffs.saveFailed') });
    } finally {
      setSaving(false);
    }
  };

  if (loading) return <Box sx={{ display: 'flex', justifyContent: 'center', py: 10 }}><CircularProgress aria-label={t('common.loading')} /></Box>;
  if (!plan) return <Alert severity="error">{msg?.text ?? t('tariffs.planUnavailable')}</Alert>;

  return (
    <Box>
      <Typography variant="h4" fontWeight={800} sx={{ mb: 1.5 }}>{t('tariffs.title')}</Typography>
      {/* One save bar for every form (HEU-R01): sticky, says whether there are edits, «Отменить»
          next to «Сохранить» — the same model as Settings. */}
      <Stack direction="row" alignItems="center" flexWrap="wrap" columnGap={1.5} rowGap={1} sx={{
        position: 'sticky', top: 0, zIndex: 5, mb: 2, p: 1.5, borderRadius: 3, bgcolor: 'background.paper',
        border: 1, borderColor: dirty ? 'primary.main' : 'divider' }}>
        <Typography variant="subtitle2" fontWeight={700}>
          {hasErrors ? t('form.fixErrors') : dirty ? t('tariffs.unsaved') : t('reconcile.noUnsaved')}
        </Typography>
        <Box sx={{ ml: 'auto', display: 'flex', gap: 1 }}>
          <Button size="small" disabled={!dirty || saving} onClick={() => { setEnv(JSON.parse(orig)); setErrors({}); setMsg(null); }}>{t('common.cancel')}</Button>
          <Button size="small" variant="contained" onClick={save} disabled={saving || !dirty || hasErrors}>
            {saving ? t('common.saving') : t('common.save')}
          </Button>
        </Box>
      </Stack>
      {msg && <Alert severity={msg.sev} sx={{ mb: 2 }}>{msg.text}</Alert>}

      <Box sx={{ mb: 2 }}>
        <Card>
          <Stack direction="row" alignItems="center" spacing={1.5} mb={2}>
            <Typography variant="h6" fontWeight={700}>{plan.plan_name}</Typography>
            {plan.is_active && <StatusChip status="live" label={t('tariffs.active')} />}
          </Stack>
          {/* An expired validity is said, not hidden behind «активен» (TAR-19): the hub keeps
              pricing with the plan, so the chip is true — the date is the news. */}
          {(plan.valid_until_timestamp ?? 0) > 1_600_000_000 && (plan.valid_until_timestamp as number) * 1000 < Date.now() && (
            <Alert severity="warning" sx={{ mb: 2 }}>
              {t('tariffs.expired', { date: new Date((plan.valid_until_timestamp as number) * 1000).toLocaleDateString(dateLocale(), { day: 'numeric', month: 'long', year: 'numeric' }) })}
            </Alert>
          )}
          <Grid container spacing={2} sx={{ maxWidth: 680 }}>
            <Grid item xs={12} sm={6}><FormField required label={t('tariffs.planName')} value={plan.plan_name ?? ''} onChange={(v) => updatePlan({ plan_name: v })} /></Grid>
            <Grid item xs={6} sm={3}>
              <FormField
                label={t('tariffs.currency')} value={plan.currency_code ?? ''} onChange={(v) => updatePlan({ currency_code: v })}
                options={[...CURRENCIES, ...(plan.currency_code && !CURRENCIES.includes(plan.currency_code) ? [plan.currency_code] : [])]
                  .map((c) => ({ value: c, label: c }))}
              />
            </Grid>
            <Grid item xs={6} sm={3}>
              <DecimalField
                label={t('tariffs.vat')} value={plan.vat_percent} min={0} max={100} decimals={2} helper={t('tariffs.vatHint')}
                onChange={(v, e) => { setErr('vat', e); if (v !== null) updatePlan({ vat_percent: v }); }}
              />
            </Grid>
            <Grid item xs={12} sm={6}><FormField label={t('tariffs.provider')} value={plan.provider_name ?? ''} onChange={(v) => updatePlan({ provider_name: v })} /></Grid>
            <Grid item xs={12} sm={6}>
              <FormField
                label={t('tariffs.planType')}
                value={plan.plan_type ?? ''}
                onChange={(v) => updatePlan({ plan_type: v as TariffPlanType })}
                options={TARIFF_PLAN_TYPE_NAMES.map((pt) => ({ value: pt, label: tCode('planType', pt) }))}
              />
            </Grid>
            <Grid item xs={12} sm={6}>
              <FormField
                label={t('tariffs.defaultZone')}
                value={plan.default_zone_id != null ? String(plan.default_zone_id) : ''}
                onChange={(v) => updatePlan({ default_zone_id: v })}
                options={[
                  ...(plan.zones ?? []).map((z) => ({ value: String(z.zone_id), label: zoneDisplayName(z.zone_id, z.zone_name) })),
                  ...(plan.default_zone_id != null && !(plan.zones ?? []).some((z) => String(z.zone_id) === String(plan.default_zone_id))
                    ? [{ value: String(plan.default_zone_id), label: `${tCode('zone', String(plan.default_zone_id))} (${t('tariffs.defaultZoneNone')})` }] : []),
                ]}
              />
              <Typography variant="caption" color="text.secondary">{t('tariffs.defaultZoneHint')}<HelpTip topic="defaultZone" /></Typography>
            </Grid>
            <Grid item xs={12}>
              <Stack direction="row" alignItems="center">
                <ToggleRow label={t('tariffs.netMetering')} checked={!!plan.net_metering_enabled} onChange={(v) => updatePlan({ net_metering_enabled: v })} />
                <HelpTip topic="netMetering" />
              </Stack>
            </Grid>
            {!plan.net_metering_enabled && (plan.zones ?? []).some((z) => z.is_enabled && (z.rate_selling_per_kwh ?? 0) > 0) && (
              <Grid item xs={12}>
                <Typography variant="caption" color="text.secondary">{t('tariffs.sellWithoutNm')}</Typography>
              </Grid>
            )}
            {plan.net_metering_enabled && (
              <>
                {/* The hub prices an export by the zone's own selling price; only a zone WITHOUT one uses
                    buy × this ratio (root, TAR-R05). The ratio also scales every surplus month banked into the
                    kWh credit (≤ 0 → 1; root net-metering-ratio-hidden), so it stays visible while net metering
                    is on — with every zone priced, that is its only job. */}
                <Grid item xs={12} sm={8}>
                  <DecimalField
                    label={t('tariffs.sellRatio')} value={plan.net_metering_default_ratio} min={0} max={1} decimals={4} required={false}
                    helper={<>{t((plan.zones ?? []).some((z) => z.is_enabled && z.rate_selling_per_kwh == null) ? 'tariffs.ratioHint' : 'tariffs.ratioCreditOnly')}<HelpTip topic="sellRatio" /></>}
                    onChange={(v, e) => { setErr('ratio', e); if (v !== null) updatePlan({ net_metering_default_ratio: v }); }}
                  />
                </Grid>
                {!(plan.zones ?? []).some((z) => z.rate_selling_per_kwh !== undefined) && plan.default_selling_rate_per_kwh != null && (
                  <Grid item xs={6} sm={4}><InfoField label={t('tariffs.sellPriceComputed')}
                    value={`${Number(plan.default_selling_rate_per_kwh).toLocaleString(intlLocale(), { maximumFractionDigits: rateDecimals })} ${plan.currency_code ?? ''}/${t('units.kWh')}`} /></Grid>
                )}
                <Grid item xs={12}>
                  <Typography variant="caption" color="text.secondary">{t('tariffs.netMeteringExplain')}</Typography>
                </Grid>
              </>
            )}
          </Grid>
        </Card>
      </Box>

      <Stack direction="row" alignItems="center">
        <Typography variant="overline" color="text.secondary">{t('tariffs.zones', { n: plan.zones?.length ?? 0 })}</Typography>
        <Button size="small" startIcon={<AddIcon />} sx={{ ml: 'auto' }} onClick={addZone}
          disabled={(plan.zones?.length ?? 0) >= ZONE_TYPES_OFFERED.length}>
          {t('tariffs.addZone')}
        </Button>
      </Stack>
      {/* plan_type is informational on the hub (root) — say when it disagrees with the zones (TAR-02). */}
      {(() => {
        const n = planZones.filter((z) => z.is_enabled).length;
        const expect: Record<string, number> = { SIMPLE: 1, TWO_ZONE: 2, THREE_ZONE: 3 };
        const want = plan.plan_type ? expect[String(plan.plan_type)] : undefined;
        return want != null && want !== n
          ? <Alert severity="info" sx={{ mt: 1 }}
              action={want === 1 && n > 1 ? <Button color="inherit" size="small" onClick={() => updatePlan(toSingleZone(plan))} sx={{ whiteSpace: 'nowrap' }}>{t('tariffs.toSingleZone')}</Button> : undefined}>
              {t('tariffs.planTypeMismatch', { type: tCode('planType', String(plan.plan_type)), n })}
            </Alert>
          : null;
      })()}
      {/* The week as the hub prices it (TAR-03/18, W2-3): who wins each hour on each day. */}
      <Typography variant="caption" color="text.secondary" sx={{ display: 'block', mt: 1 }}>{t('tariffs.weekPicture')}</Typography>
      <Stack spacing={0.5} sx={{ mt: 0.5 }}>
        {weekRows.map(({ label, row }) => (
          <Stack key={label} direction="row" alignItems="center" spacing={1}>
            <Typography sx={{ width: sameWeek ? 'auto' : 24, fontSize: 11, color: 'text.secondary', flexShrink: 0, display: sameWeek ? 'none' : 'block' }}>{label}</Typography>
            <Box sx={{ position: 'relative', flex: 1, height: sameWeek ? 22 : 14, borderRadius: 1, overflow: 'hidden', bgcolor: 'action.hover' }}>
              {runsOf(row).filter((r) => r.z >= 0).map((r) => (
                <Box key={r.a} title={`${zName(r.z)} ${hhmm(r.a)}–${hhmm(r.b)}`}
                  sx={{ position: 'absolute', top: 0, bottom: 0, left: `${(r.a / 1440) * 100}%`, width: `${((r.b - r.a) / 1440) * 100}%`,
                    bgcolor: zoneColor(r.z), borderRight: '2px solid', borderColor: 'background.paper' }} />
              ))}
            </Box>
          </Stack>
        ))}
      </Stack>
      <Stack direction="row" justifyContent="space-between" sx={{ fontSize: 11, color: 'text.secondary', mb: 0.5, pl: sameWeek ? 0 : 4 }}>
        <span>00:00</span><span>06:00</span><span>12:00</span><span>18:00</span><span>24:00</span>
      </Stack>
      <Stack direction="row" spacing={2} flexWrap="wrap" useFlexGap sx={{ mb: 1, fontSize: 12 }}>
        {planZones.map((z, i) => z.is_enabled && (
          <Stack key={i} direction="row" alignItems="center" spacing={0.75}>
            <Box sx={{ width: 10, height: 10, borderRadius: '2px', bgcolor: zoneColor(i) }} />
            <span>{zName(i)}</span>
          </Stack>
        ))}
      </Stack>
      {coverage.gaps.length > 0 && (
        coverage.fallback === null
          ? <Alert severity="error" sx={{ mb: 1 }}>{t('tariffs.coverageGaps', { spans: coverage.gaps.map(spanText).join(', ') })}</Alert>
          : <Alert severity="info" sx={{ mb: 1 }}>{t('tariffs.coverageGapsFallback', { spans: coverage.gaps.map(spanText).join(', '), zone: zName(coverage.fallback) })}</Alert>
      )}
      {coverage.overlaps.map((o) => (
        <Alert key={`${o.days}-${o.start}-${o.winner}`} severity="info" sx={{ mb: 1 }}>
          {t('tariffs.coverageOverlaps', { spans: spanText(o), winner: zName(o.winner), losers: o.losers.map(zName).join(', ') })}
        </Alert>
      ))}
      {coverage.shadowed.map((sh) => (
        <Alert key={`sh${sh.zone}`} severity="warning" sx={{ mb: 1 }}>
          {sh.by.length ? t('tariffs.zoneShadowed', { zone: zName(sh.zone), by: sh.by.map(zName).join(', ') }) : t('tariffs.zoneShadowedNone', { zone: zName(sh.zone) })}
        </Alert>
      ))}
      {coverage.overlaps.length > 0 && (
        <Typography variant="caption" color="text.secondary" sx={{ display: 'block', mb: 1 }}>{t('tariffs.priorityHint')}</Typography>
      )}
      <Grid container spacing={2} sx={{ mt: 0, mb: 2 }}>
        {(plan.zones ?? []).map((z, i) => (
          <Grid item xs={12} md={6} key={`${z.zone_id}-${i}`}>
            <Card outlined>
              <Stack direction="row" alignItems="center" spacing={1} mb={1.5}>
                <Typography variant="subtitle1" fontWeight={700}>{zoneDisplayName(z.zone_id, z.zone_name)}</Typography>
                {(liveZone ? z.zone_id === liveZone : z.is_currently_active) && <StatusChip status="live" label={t('tariffs.now')} />}
                <Box sx={{ ml: 'auto' }}>
                  <ToggleRow label={t('tariffs.enabled')} checked={!!z.is_enabled} onChange={(v) => updateZone(i, { is_enabled: v })} />
                </Box>
                <IconButton size="small" aria-label={t('tariffs.removeZoneTitle', { name: zoneDisplayName(z.zone_id, z.zone_name) })}
                  onClick={() => removeZone(i)} disabled={(plan.zones?.length ?? 0) <= 1}>
                  <DeleteOutlineIcon fontSize="small" />
                </IconButton>
              </Stack>
              <DecimalField
                label={t('tariffs.rate', { cur: plan.currency_code ?? '' })} value={z.rate_buying_per_kwh} min={0} max={1000} decimals={rateDecimals}
                onChange={(v, e) => { setErr(`rate${i}`, e); if (v !== null) updateZone(i, { rate_buying_per_kwh: v }); }}
              />
              {/* On every zone, whatever the net-metering toggle: the hub values exports at it with net metering
                  off too (root via content 04.11 / 06.11) — an export-only tariff (SEG) sets it here and leaves
                  net metering off (content sellprice-gated-on-netmetering). The hub always reports it. */}
              <Stack direction="row" spacing={1.5} sx={{ mt: 1.5 }}>
                <DecimalField
                  label={t('tariffs.sellRate', { cur: plan.currency_code ?? '' })} value={z.rate_selling_per_kwh} min={0} max={1000} decimals={rateDecimals} required={false}
                  onChange={(v, e) => { setErr(`sell${i}`, e); if (v !== null) updateZone(i, { rate_selling_per_kwh: v }); }}
                />
                {/* The per-zone credit ratio isn't shown: the v1.1.x hub settles net metering by the
                    plan-wide ratio only (root 02.11) — the value is still echoed on a save. */}
              </Stack>
              <Box sx={{ mt: 1.5 }}>
                <FormField
                  label={t('tariffs.zoneType')}
                  value={typeof z.zone_id === 'string' ? z.zone_id : ''}
                  // The name follows the type while it is the type's own name (TAR-R02: a zone switched to
                  // «Вечерний пик» was saved as «Утренний пик»); a name the user typed stays.
                  onChange={(v) => updateZone(i, { zone_id: v as TariffZoneId, ...(!z.zone_name || zoneDisplayName(z.zone_id, z.zone_name) === tCode('zone', String(z.zone_id)) ? { zone_name: tCode('zone', v) } : {}) })}
                  // A type another zone already has isn't offered: the hub keys zones by type (it only logs a
                  // DUPLICATE zone_id and keeps both), so two «Night» zones would mix their hours and money.
                  options={ZONE_TYPES_OFFERED.concat(TARIFF_ZONE_ID_NAMES.filter((zid) => zid === z.zone_id && !ZONE_TYPES_OFFERED.includes(zid)))
                    .filter((zid) => zid === z.zone_id || !(plan.zones ?? []).some((o, j) => j !== i && o.zone_id === zid))
                    .map((zid) => ({ value: zid, label: tCode('zone', zid) }))}
                />
              </Box>
              {/* The zone's hours — editable (was a caption). Across midnight is fine: 23:00 → 07:00. */}
              {(() => {
                const r = zoneRange(z);
                return (
                  <Stack direction="row" spacing={1.5} sx={{ mt: 1.5 }}>
                    <TimeField label={t('tariffs.zoneFrom')} minutes={r ? r.start : 0} onChange={(m) => setZoneTime(i, 'start', hhmm(m))} onError={(e) => setErr(`from${i}`, e)} />
                    <TimeField label={t('tariffs.zoneTo')} minutes={r ? r.end : 0} onChange={(m) => setZoneTime(i, 'end', hhmm(m))} onError={(e) => setErr(`to${i}`, e)} />
                  </Stack>
                );
              })()}
              {/* Days (W2-3): Mon..Sun toggles; all seven = every day. */}
              {(() => {
                const mask = zoneDays(z);
                const on = Array.from({ length: 7 }, (_, d) => d).filter((d) => mask & (1 << d)).map(String);
                return (
                  <Box sx={{ mt: 1.5 }}>
                    <Typography variant="caption" color="text.secondary">{t('tariffs.zoneDays')} · {daysText(mask, dayShort, t('tariffs.everyDay')) || '—'}{i === 0 && <HelpTip topic="zoneDays" />}</Typography>
                    <ToggleButtonGroup size="small" value={on} aria-label={t('tariffs.zoneDays')} sx={{ display: 'flex', mt: 0.5, '& .MuiToggleButton-root': { flex: 1, minWidth: 0, px: 0.5, py: 0.5 },
                      '& .MuiToggleButton-root.Mui-selected, & .MuiToggleButton-root.Mui-selected:hover': { bgcolor: 'primary.main', color: 'primary.contrastText' } }}
                      onChange={(_, v: string[]) => setZoneDays(i, v.reduce((m, d) => m | (1 << Number(d)), 0))}>
                      {dayShort.map((lbl, d) => <ToggleButton key={d} value={String(d)} aria-label={lbl}>{lbl}</ToggleButton>)}
                    </ToggleButtonGroup>
                    {z.is_enabled && mask === 0 && <Typography variant="caption" color="error" sx={{ display: 'block', mt: 0.5 }}>{t('tariffs.zoneNoDays')}</Typography>}
                  </Box>
                );
              })()}
              {(z.conditions ?? []).some((c) => !['TIME_RANGE', 'WEEKDAY', 'WEEKEND'].includes(c.condition_type)) && (
                <Typography variant="caption" color="text.secondary" sx={{ display: 'block', mt: 1 }}>
                  {(z.conditions ?? []).filter((c) => !['TIME_RANGE', 'WEEKDAY', 'WEEKEND'].includes(c.condition_type)).map((c) => condLabel(c)).join(' · ')}
                </Typography>
              )}
            </Card>
          </Grid>
        ))}
      </Grid>

      <Card outlined>
        <Typography variant="overline" color="text.secondary">{t('tariffs.taxes')}</Typography>
        <Stack direction="row" spacing={3} flexWrap="wrap" useFlexGap sx={{ mt: 1 }}>
        </Stack>
        {/* Editable per-kWh charges and fixed payments (TAR-15: read-only, «Фиксированные платежи: 1»
            hid the 120 ₽ subscription). */}
        <Grid container spacing={2} sx={{ mt: 0, maxWidth: 680 }}>
          {(['energy_tax_per_kwh', 'grid_usage_fee_per_kwh', 'renewable_levy_per_kwh'] as const).map((k) => (
            <Grid item xs={12} sm={4} key={k}>
              <DecimalField
                label={`${t(k === 'energy_tax_per_kwh' ? 'tariffs.energyTax' : k === 'grid_usage_fee_per_kwh' ? 'tariffs.gridFee' : 'tariffs.renewableLevy').replace(/\s*\/(кВт·ч|kWh)$/, '')}, ${plan.currency_code ?? ''}/${t('units.kWh')}`}
                value={plan[k] ?? 0} min={0} max={1000} decimals={rateDecimals}
                onChange={(v, e) => { setErr(k, e); if (v !== null) updatePlan({ [k]: v } as Partial<TariffPlan>); }}
              />
            </Grid>
          ))}
          {/* Fixed payments aren't shown: no cost on a v1.1.x hub includes them (root 02.11 — the field
              would change nothing); the plan keeps them and a save echoes them. */}
          {SHOW_FIXED_PAYMENTS && (plan.fixed_payments ?? []).map((fp, i) => (
            <Grid item xs={12} sm={6} key={`fp-${i}`}>
              <DecimalField
                label={`${fp.payment_name || t('tariffs.fixedPayment')}, ${fp.currency || plan.currency_code || ''} / ${t(fp.billing_period === 'YEARLY' ? 'tariffs.perYear' : 'tariffs.perMonth')}`}
                value={fp.amount} min={0} max={1000000} decimals={2}
                onChange={(v, e) => {
                  setErr(`fp${i}`, e);
                  if (v !== null) updatePlan({ fixed_payments: (plan.fixed_payments ?? []).map((x, j) => (j === i ? { ...x, amount: v } : x)) });
                }}
              />
            </Grid>
          ))}
        </Grid>
        <Stack direction="row" spacing={3} flexWrap="wrap" useFlexGap sx={{ mt: 1.5 }}>
          <InfoField label={t('tariffs.progressive')} value={plan.progressive_config?.is_enabled ? t('tariffs.on') : t('tariffs.off')} />
        </Stack>
      </Card>
    </Box>
  );
};
