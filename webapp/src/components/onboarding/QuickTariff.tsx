/**
 * QuickTariff — the wizard's tariff: the price as the bill prints it, saved in one press
 * (W191-07/08). The full editor stays on «Tariffs» for zones, levies, standing charges.
 */
import React, { useEffect, useState } from 'react';
import { Alert, Button, FormControlLabel, MenuItem, Stack, Switch, TextField, Typography } from '@mui/material';
import { getApiClient } from '../../services/api';
import { BaseResponse } from '../../types/api';
import { TariffPlan, TariffPlanConfigData } from '../../types/tariff-plan';
import { rate as fmtRate } from '../common/ruFormat';
import { quickPlan, netOf, richPlan } from './quickTariffPlan';
import { useConfirm } from '../common/ConfirmDialog';
import { t, getLang } from '../../i18n';

const CURRENCIES = ['EUR', 'RUB', 'USD', 'GBP', 'CHF', 'PLN', 'CZK', 'UAH', 'KZT', 'JPY', 'CNY'];

const num = (s: string): number | null => {
  const v = Number(s.replace(',', '.').trim());
  return s.trim() !== '' && Number.isFinite(v) && v >= 0 ? v : null;
};

/** `factory`: the hub still runs its factory example plan (telemetry tariff.placeholder) — replacing it needs no question. */
export const QuickTariff: React.FC<{ onSaved?: () => void; factory?: boolean }> = ({ onSaved, factory }) => {
  const [env, setEnv] = useState<BaseResponse<TariffPlanConfigData> | null>(null);
  const [currency, setCurrency] = useState('EUR');
  const [price, setPrice] = useState('');
  const [twoZone, setTwoZone] = useState(false);
  const [night, setNight] = useState('');
  // ON by default — content's export article tells readers to type the bill's price; changing this
  // default → tell content.rbgrid first (sellprice-gated-on-netmetering 06.11).
  const [inclVat, setInclVat] = useState(true);
  const [vat, setVat] = useState('');
  const [sell, setSell] = useState('');
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState<{ ok: boolean; text: string } | null>(null);
  const confirm = useConfirm();

  useEffect(() => {
    let on = true;
    getApiClient().getTariffPlans().then((e) => {
      if (!on) return;
      setEnv(e);
      const p = e?.data?.list?.[0] as TariffPlan | undefined;
      setCurrency(p?.currency_code || (getLang() === 'ru' ? 'RUB' : 'EUR'));
      setVat(p?.vat_percent != null ? String(p.vat_percent) : '0');
    }).catch(() => { /* offline — the save will say so */ });
    return () => { on = false; };
  }, []);

  const p = num(price), n = twoZone ? num(night) : null, v = num(vat) ?? 0, s = num(sell);
  const ready = p != null && p > 0 && (!twoZone || (n != null && n > 0));
  const gross = (x: number) => (inclVat ? x : x * (1 + v / 100));

  const save = async () => {
    if (!ready || p == null) return;
    const current = env?.data?.list?.[0] as TariffPlan | undefined;
    if (!factory && richPlan(current) && !(await confirm({ title: t('quickTariff.replaceTitle'), body: t('quickTariff.replaceBody'), confirmLabel: t('quickTariff.replace'), danger: true }))) return;
    setBusy(true); setMsg(null);
    try {
      const fresh = env ?? await getApiClient().getTariffPlans();
      const base = fresh?.data?.list?.[0] as TariffPlan | undefined;
      const plan = quickPlan(base, { currency, price: p, nightPrice: twoZone ? n : null, inclVat, vatPercent: v, sellPrice: s });
      const out = { ...(fresh ?? {}), data: { ...((fresh?.data as object) ?? {}), list: [plan, ...((fresh?.data?.list ?? []).slice(1))] } } as BaseResponse<TariffPlanConfigData>;
      await getApiClient().saveTariffPlans(out);
      setEnv(out);
      setMsg({ ok: true, text: t('quickTariff.saved') });
      onSaved?.();
    } catch {
      setMsg({ ok: false, text: t('quickTariff.failed') });
    } finally { setBusy(false); }
  };

  return (
    <Stack spacing={1.5}>
      <Stack direction={{ xs: 'column', sm: 'row' }} spacing={1.5}>
        <TextField select size="small" label={t('quickTariff.currency')} value={currency} onChange={(e) => setCurrency(e.target.value)} sx={{ minWidth: 110 }}>
          {Array.from(new Set([currency, ...CURRENCIES])).map((c) => <MenuItem key={c} value={c}>{c}</MenuItem>)}
        </TextField>
        <TextField size="small" label={twoZone ? t('quickTariff.dayPrice', { cur: currency }) : t('quickTariff.price', { cur: currency })} value={price}
          onChange={(e) => setPrice(e.target.value)} inputProps={{ inputMode: 'decimal' }} placeholder="0,35" />
        {twoZone && <TextField size="small" label={t('quickTariff.nightPrice', { cur: currency })} value={night} onChange={(e) => setNight(e.target.value)} inputProps={{ inputMode: 'decimal' }} />}
      </Stack>
      <FormControlLabel control={<Switch checked={twoZone} onChange={(e) => setTwoZone(e.target.checked)} />} label={t('quickTariff.twoZone')} />
      <Stack direction={{ xs: 'column', sm: 'row' }} spacing={1.5} alignItems={{ sm: 'center' }}>
        <FormControlLabel control={<Switch checked={inclVat} onChange={(e) => setInclVat(e.target.checked)} />} label={t('quickTariff.inclVat')} />
        <TextField size="small" label={t('quickTariff.vat')} value={vat} onChange={(e) => setVat(e.target.value)} inputProps={{ inputMode: 'decimal' }} sx={{ maxWidth: 120 }} />
      </Stack>
      <TextField size="small" label={t('quickTariff.sell', { cur: currency })} value={sell} onChange={(e) => setSell(e.target.value)} inputProps={{ inputMode: 'decimal' }} helperText={t('quickTariff.sellHint')} sx={{ maxWidth: 360 }} />
      {/* The «Total» the hub will count with, before saving (W191-08: nowhere to be seen). */}
      {ready && p != null && (
        <Typography sx={{ fontVariantNumeric: 'tabular-nums' }}>
          {twoZone && n != null
            ? t('quickTariff.totalTwo', { day: fmtRate(gross(p), currency), night: fmtRate(gross(n), currency) })
            : t('quickTariff.total', { price: fmtRate(gross(p), currency) })}
          {v > 0 && <Typography component="span" sx={{ color: 'text.secondary', fontSize: 13 }}>{' '}{t('quickTariff.stored', { net: fmtRate(netOf(p, inclVat, v), currency), vat: v })}</Typography>}
        </Typography>
      )}
      <Stack direction="row" spacing={1} alignItems="center">
        <Button variant="contained" disabled={!ready || busy} onClick={save}>{busy ? t('quickTariff.saving') : t('quickTariff.save')}</Button>
      </Stack>
      {msg && <Alert severity={msg.ok ? 'success' : 'error'}>{msg.text}</Alert>}
    </Stack>
  );
};
