/**
 * SavingsCard — «Экономия»: concrete, computed advice (review 10 §3, personas 05–09 «деньги»).
 * Shows only insights backed by device data; every figure is an estimate and says so («≈»).
 *   • «Сейчас дорого»  — live zone/rate (system.tariff) vs the plan's cheapest zone + time to switch;
 *   • «Зарядка в дорогой зоне» — yesterday's EV charging by the hour, priced by its zones: what the
 *     same energy would have cost in the cheapest zone (a fact from the plan, not a forecast);
 *   • «Куда уходят деньги сегодня» — each line's share of the input's energy × today's cost;
 *   • «Прогноз счёта»  — month-to-date cost (summary period=month) extrapolated;
 *   • «Сегодня к вчера» — today's run-rate vs yesterday (history economics, day);
 *   • «Фоновое потребление» — minimum input power over the last day × a month × rate.
 * «Мощность ⇄ Деньги»: in power mode the same insights read in kWh (where the energy goes, the
 * month's consumption forecast, today vs yesterday by energy, base load in kWh/month); the
 * «Сейчас дорого» tariff advice is about prices and reads the same in both modes.
 */
import { StackedBar } from '../charts/StackedBar';
import React, { useMemo, useState } from 'react';
import { Box, Button, Stack, Typography } from '@mui/material';
import { VampireHuntDialog } from './VampireHuntDialog';
import { Card } from '@rbgrid/design-system';
import { useDsTokens } from '../../theme/useDsTokens';
import { useHistory } from '../../hooks/useHistory';
import { useInputSeries } from '../../hooks/useInputSeries';
import { useTariffPlan } from '../../hooks/useTariffPlan';
import { UnitEntityGroup, unitMetricValue } from '../../entities/model';
import { UnitsConfigJoin } from '../../hooks/useUnitsConfig';
import { lineUnits, fullNameOf, isGenerationUnit, mainUnit, isStation } from '../../pages/overviewData';
import { signedPowerW } from '../../entities/siteModel';
import { zoneTimeline, minutesUntilRate } from '../../economics/tariffPlan';
import { useDeviceClock, formatDeviceTime } from '../../core/deviceTime';
import { expensiveNow, billShares, monthForecast, dearZoneCharge } from '../../economics/insights';
import { useYesterdayHourly, useHourPowerSum } from '../../hooks/useYesterdayHourly';
import { useDeviceGroups } from '../../hooks/useDeviceGroups';
import { standbyFloor, weekAverageRate, monthlyKwh, isNotableRise } from '../../economics/vampires';
import { usePeriodTotals } from '../../hooks/usePeriodTotals';
import { useVsYesterday } from '../../hooks/useVsYesterday';
import { LiveRate, solarValue, grossOf } from '../../economics/costRate';
import { nf, pwW, enWh, zoneLabel } from '../common/ruFormat';
import { useMoneyLens } from '../../economics/MoneyLens';
import type { HistoryRecord } from '../../types/history';
import { t, intlLocale, dateLocale } from '../../i18n';

export interface SavingsCardProps {
  /** No link to the hub: say so instead of «tips appear once data and a tariff are set» (L3P-R07). */
  lost?: boolean;
  /** The hub's own standby floor (system.base_w / base_w_nights), preferred when it covers ≥ 3 nights. */
  hubBase?: { w?: number; nights?: number } | null;
  groups: UnitEntityGroup[];
  join: UnitsConfigJoin;
  /** REST id of the main input (HEX4), for summary/history. */
  mainId: string | null;
  /** Input's energy today (Wh) and today's cost from the device. */
  inputDayWh: number | null;
  costDay: number | null;
  /** The site's export today (Wh) and the hub's revenue for it — for the solar line. */
  exportDayWh?: number | null;
  revDay?: number | null;
  rate: LiveRate;
  fmt: (v: number | null | undefined) => string;
  /** «Предприятие»: site wording instead of the household one (3φ audit I3P-17). */
  industrial?: boolean;
}

const hm = (min: number) => (min >= 60
  ? `${Math.floor(min / 60)} ${t('units.h')} ${min % 60} ${t('units.min')}`
  : `${min} ${t('units.min')}`);

const genUnitsNow = (groups: UnitEntityGroup[], join: UnitsConfigJoin): boolean => groups.some((g) => g.present && isGenerationUnit(g, join));

export const SavingsCard: React.FC<SavingsCardProps> = ({ groups, join, mainId, inputDayWh, costDay, exportDayWh, revDay, rate, fmt, hubBase, industrial, lost }) => {
  const ds = useDsTokens();
  const { mode: lensMode, dayRate } = useMoneyLens();
  const money = lensMode === 'money';
  const { plan } = useTariffPlan();
  // A week of hour records for the standby floor (home vampires: the median over nights).
  const weekRange = useMemo(() => { const n = Math.floor(Date.now() / 3_600_000) * 3600; return { from: n - 8 * 86400, to: n + 3600 }; }, []);
  const hourEl = useHistory(mainId, 'hour', 'electrical', 200, weekRange);
  const now = new Date();

  const clock = useDeviceClock();
  // Today's zones by the hub's weekday (Mon-origin, as the hub's weekday masks).
  const hubDay = (() => { const p = clock.parts(Math.floor(now.getTime() / 1000)); return (new Date(Date.UTC(p.y, p.mo - 1, p.d)).getUTCDay() + 6) % 7; })();
  // Zone rates on the live rate's basis (+ levies, VAT when the live rate carries them — L3P-02):
  // a gross 0.39 against a net night 0.20 read «2.0× dearer» instead of 1.45×.
  const gross = useMemo(() => (rate.gross ? grossOf(plan) : null), [rate.gross, plan]);
  const spans = useMemo(() => {
    const sp = zoneTimeline(plan, hubDay);
    return gross ? sp.map((x) => ({ ...x, rate: gross(x.rate) })) : sp;
  }, [plan, hubDay, gross]);
  const dear = expensiveNow(rate.buy, spans);
  // The cheap zone's hours today, merged across midnight (HOME-18: the advice named no hours).
  const cheapHours = dear ? (() => {
    const c = spans.filter((sp) => Math.abs(sp.rate - dear.cheapestRate) < 1e-9).map((sp) => [sp.from, sp.to]);
    const first = c.find(([a]) => a === 0), last = c.find(([, b]) => b === 1440);
    const parts = first && last && first !== last ? [[last[0], first[1]], ...c.filter((x) => x !== first && x !== last)] : c;
    const hh = (m: number) => `${String(Math.floor((m % 1440) / 60)).padStart(2, '0')}:${String(m % 60).padStart(2, '0')}`;
    return parts.map(([a, b]) => `${hh(a)}–${hh(b)}`).join(', ');
  })() : '';
  // Time until the CHEAP zone starts — not the next change of any zone (TAR-R04).
  const toSwitch = dear ? minutesUntilRate(spans, clock.parts(Math.floor(now.getTime() / 1000)), dear.cheapestRate) : null;

  // Yesterday's charging (every EV point — a three-phase charger is three) priced by yesterday's
  // zones on the same gross basis as the advice above.
  const evUnits = groups.filter((g) => isStation(g, join));
  const evIds = evUnits.map((g) => join.forGroup(g)?.hex).filter((x): x is string => !!x).sort();
  const evYesterday = useYesterdayHourly(industrial ? [] : evIds);
  const yesterdaySpans = useMemo(() => {
    const sp = zoneTimeline(plan, (hubDay + 6) % 7);
    return gross ? sp.map((x) => ({ ...x, rate: gross(x.rate) })) : sp;
  }, [plan, hubDay, gross]);
  const evDear = evYesterday ? dearZoneCharge(evYesterday, yesterdaySpans) : null;

  // One appliance on several phases — a group whose points share one role (a three-phase charger,
  // a heat pump) — is one share, not three thirds that each fall under «other points».
  const { names: groupNames } = useDeviceGroups();
  const lines = lineUnits(groups, join).filter((g) => !isGenerationUnit(g, join))
    .map((g) => ({ name: fullNameOf(g, join), wh: unitMetricValue(g, 'energy_day_in'), key: g.unitKey, gid: join.forGroup(g)?.groupId, role: join.forGroup(g)?.role }));
  const appliances = new Map<number, typeof lines>();
  lines.forEach((l) => { if (l.gid != null) appliances.set(l.gid, [...(appliances.get(l.gid) ?? []), l]); });
  const isAppliance = (gid?: number) => {
    const m = gid != null ? appliances.get(gid) : undefined;
    return !!m && m.length > 1 && !!m[0].role && m.every((x) => x.role === m[0].role) && !!groupNames.get(gid as number);
  };
  const shares = billShares(
    [
      ...lines.filter((l) => !isAppliance(l.gid)),
      ...Array.from(appliances.entries()).filter(([gid]) => isAppliance(gid))
        .map(([gid, m]) => ({ name: groupNames.get(gid) as string, wh: m.reduce((s, x) => s + (x.wh ?? 0), 0), key: `group:${gid}` })),
    ],
    inputDayWh,
    costDay,
  );

  // Month to date from the hub's CLOSED records — the same figure as Economics «месяц»
  // (rerun 16.10 HOME-R05/HA-R04: /summary without from/to summed the whole ring, a 3rd source).
  // A zero month-to-date is «no data yet», not «free» — no forecast (RES-04/HOME-04).
  // The same month as the Today card's «Month to date»: closed days + TODAY BY THE DAY COUNTERS
  // (L3P-03: the forecast used today's closed hours — another base than the figure next to it).
  const todayCounters = useMemo(() => (inputDayWh != null ? { inWh: Math.round(inputDayWh), outWh: Math.round(exportDayWh ?? 0), cost: Math.round((costDay ?? 0) * 100) / 100, revenue: Math.round((revDay ?? 0) * 100) / 100, n: 1 } : null), [inputDayWh, exportDayWh, costDay, revDay]);
  const inputSeries = useInputSeries(mainId);
  const mtd = usePeriodTotals(inputSeries, 'month', { today: todayCounters });
  // Not before the month's records arrive: «month» = today for a few seconds read a forecast of
  // €2.19 (L3P-04).
  const mtdVal = mtd && !mtd.loading && mtd.n > 0 ? (money ? mtd.cost : mtd.inWh / 1000) : 0;
  const coveredDays = mtd?.since != null ? (now.getTime() / 1000 - mtd.since) / 86400 : null;
  const forecast = mtdVal > 0 ? monthForecast(mtdVal, now, coveredDays) : null;

  // «к вчера (к этому часу)»: today's closed hours vs the same hours yesterday (useVsYesterday —
  // the same rule as the History; HOME-R03).
  const vsY = useVsYesterday(inputSeries, money);
  /** A month/day figure in the lens' unit (money or energy; energy forecasts come in kWh / Wh). */
  const kwhText = (kwh: number) => enWh(kwh * 1000);

  // «Домашние вампиры» (documents/design/HOME_VAMPIRES.md): the standby floor = the median over
  // nights of the 01–05 h hourly means — not the lowest instant of a day (one dip set it before).
  // Priced at the week-average buying price (a standby load runs round the clock), with its share
  // of the month and the trend of the last nights.
  // A heat pump or a charger running at night is a controlled load, not standby: their metered
  // hours come off the input's before the floor is taken (site team 02.11 — a heat-pump house
  // read «standby 1.4 kW»).
  const controlled = lineUnits(groups, join).filter((g) => isStation(g, join) || join.forGroup(g)?.role === 'HEATING_COOLING');
  const controlledIds = controlled.map((g) => join.forGroup(g)?.hex).filter((x): x is string => !!x).sort();
  const controlledW = useHourPowerSum(controlledIds, weekRange.from, weekRange.to);
  const controlledNames = Array.from(new Set(controlled.map((g) => {
    const gid = join.forGroup(g)?.groupId;
    return gid != null && isAppliance(gid) ? (groupNames.get(gid) as string) : fullNameOf(g, join);
  })));
  const floorWeb = useMemo(() => {
    const recs = hourEl.records as HistoryRecord[];
    if (!controlledW) return standbyFloor(recs, clock);
    return standbyFloor(recs.map((r) => {
      const c = controlledW.get(r.ts);
      const p = (r as { p_act?: number }).p_act;
      return c != null && typeof p === 'number' ? ({ ...r, p_act: Math.max(0, p - c) } as HistoryRecord) : r;
    }), clock);
  }, [hourEl.records, clock, controlledW]);
  // One value for the app and HA: the hub's own floor when it has one over ≥ 3 nights; the trend
  // still comes from the records.
  const floor = hubBase?.w != null && (hubBase.nights ?? 0) >= 3
    ? { w: hubBase.w, nights: hubBase.nights ?? 3, trendW: floorWeb.trendW }
    : floorWeb;
  const weekNet = plan ? weekAverageRate(plan.zones ?? [], plan.default_zone_id) : null;
  const weekRate = weekNet != null && gross ? gross(weekNet) : weekNet;
  const floorKwh = floor.w != null ? monthlyKwh(floor.w) : null;
  const floorMoney = floorKwh != null && weekRate != null ? floorKwh * weekRate : null;
  const monthKwh = mtd && mtd.n > 0 ? monthForecast(mtd.inWh / 1000, now, coveredDays)?.forecast ?? null : null;
  const floorShare = floorKwh != null && monthKwh && monthKwh > 0 ? Math.min(100, (floorKwh / monthKwh) * 100) : null;

  /** The site's wording: «…Ind» keys on an industrial site. */
  const k = (key: string) => (industrial ? `${key}Ind` : key);
  const [hunting, setHunting] = useState(false);
  // «Охота на вампиров» needs only the live input, not the nights — offered with either line.
  const huntButton = industrial ? null : (
    <Box sx={{ mt: 0.75 }}><Button size="small" variant="outlined" onClick={() => setHunting(true)}>{t('hunt.start')}</Button></Box>
  );

  const items: React.ReactNode[] = [];
  // Exporting now with generation on site → the cheapest moment to run loads (UX audit GEN-17:
  // the tariff advice said «включайте ночью» while 1 kW was leaving at the sell price).
  const mainNow = mainUnit(groups, join);
  const mainW = mainNow?.present ? signedPowerW(mainNow) : null;
  const surplusNow = genUnitsNow(groups, join) && mainW != null && mainW < -200 ? -mainW : null;
  if (surplusNow != null) {
    const lost = rate.buy != null ? (surplusNow / 1000) * (rate.buy - (rate.sell ?? 0)) : null;
    items.push(
      <Insight key="surplus" tone="tip" title={t('cards.surplusTitle', { power: pwW(surplusNow) })}>
        {t(k('cards.surplusText'))}{lost != null && lost > 0 ? ` ${t('cards.surplusGain', { money: fmt(lost) })}` : ''}
      </Insight>,
    );
  }
  if (dear && surplusNow == null) {
    items.push(
      <Insight key="dear" tone="warn" title={t('cards.savingsDearTitle', { zone: zoneLabel(rate.zone) ?? t('cards.savingsCurrentZone'), ratio: nf(dear.ratio, 1), cheapest: dear.cheapestName })}>
        {t(k('cards.savingsDearText'), { cheapest: dear.cheapestName })}{cheapHours ? ` (${cheapHours})` : ''}
        {toSwitch != null && toSwitch > 0 ? t('cards.savingsCheapIn', { zone: dear.cheapestName, time: hm(toSwitch) }) : ''}.
      </Insight>,
    );
  }
  if (evDear) {
    // The user's own label, never a guess at the appliance: the group the points share, the one
    // point's name, else the role they were given (site team 02.11 — no load recognition claimed).
    const gids = new Set(evUnits.map((g) => join.forGroup(g)?.groupId));
    const gid = gids.size === 1 ? Array.from(gids)[0] : undefined;
    const evName = gid != null && groupNames.get(gid) ? (groupNames.get(gid) as string)
      : evUnits.length === 1 ? fullNameOf(evUnits[0], join) : t('cards.evChargingRole');
    items.push(
      <Insight key="evDear" tone="warn" title={t('cards.evDearTitle', { name: evName, energy: enWh(evDear.dearWh), zone: evDear.zone })}>
        {t('cards.evDearText', { cheapest: evDear.cheapestName, money: fmt(evDear.extra) })}
      </Insight>,
    );
  }
  // Only a real breakdown: with every line silent the list is one «Остальные точки 100 %» row —
  // not «where the money goes» (3φ audit H3P-11).
  // Stable colours: the points' keys in a fixed order, not by today's rank.
  const shareKeys = shares.map((x) => x.key).filter((k): k is string => !!k).sort();
  const moneyBar = money && shares.every((x) => x.money != null);
  if (shares.length > 1) {
    items.push(
      <Insight key="shares" tone="info" title={money ? t('cards.savingsSharesTitle') : t('cards.savingsSharesTitleEnergy')}>
        {/* One bar of today's parts, the rows as its key (charts audit H: a mini bar per row made the
            reader compare lengths across rows). Colour by the point, «other» muted. */}
        <Box sx={{ mt: 0.75 }}>
          <StackedBar
            // Money only when every part has a cost — else energy for all (a Wh value drawn as € was a bug).
            fmt={(v) => (moneyBar ? `${t('common.approx')} ${fmt(v)}` : enWh(v))}
            parts={shares.map((s, i) => ({
              key: `${i}-${s.key ?? s.name}`, label: s.name, value: moneyBar ? (s.money as number) : s.wh,
              color: s.key ? ds.series[Math.max(0, shareKeys.indexOf(s.key)) % ds.series.length] : ds.inkMuted, muted: !s.key,
            }))} />
        </Box>
      </Insight>,
    );
  }
  if (forecast) {
    items.push(
      <Insight key="fc" tone="info" title={money ? t('cards.savingsForecastTitle', { money: fmt(forecast.forecast) }) : t('cards.savingsForecastTitleEnergy', { energy: kwhText(forecast.forecast) })}>
        {forecast.partial
          ? t(money ? 'cards.savingsForecastTextData' : 'cards.savingsForecastTextDataEnergy', { elapsed: nf(forecast.elapsedDays, 1), date: formatDeviceTime(clock, mtd?.since ?? 0, { day: 'numeric', month: 'short' }, dateLocale()) })
          : t(money ? 'cards.savingsForecastText' : 'cards.savingsForecastTextEnergy', { elapsed: nf(forecast.elapsedDays, 1), days: forecast.daysInMonth })}
      </Insight>,
    );
  } else if (mtdVal > 0 && coveredDays != null && coveredDays < 1) {
    // Data only since today (a new hub): say when the forecast comes rather than hide it silently.
    items.push(<Insight key="fc" tone="info" title={t('cards.savingsForecastSoonTitle')}>{t('cards.savingsForecastSoon')}</Insight>);
  }
  if (vsY && Math.abs(vsY.pct) >= 10) {
    items.push(
      <Insight key="vsy" tone={vsY.pct > 0 ? 'warn' : 'good'} title={t(`cards.savingsVsY${vsY.pct > 0 ? 'Dearer' : 'Cheaper'}${money ? '' : 'Energy'}`, { pct: nf(Math.abs(vsY.pct)) })}>
        {money ? t('cards.savingsVsYText', { today: fmt(vsY.today), yesterday: fmt(vsY.yesterday) }) : t('cards.savingsVsYTextEnergy', { today: enWh(vsY.today), yesterday: enWh(vsY.yesterday) })}
      </Insight>,
    );
  }
  // «Солнце сегодня» (UX audit GEN-05 — the prosumer's main question had no answer anywhere):
  // produced = Σ generation's day output; what left through the input was sold, the rest the
  // house used itself and did not buy — ONE formula with Economics (solarValue: the day's average
  // buying price, not the current zone — rerun 16.10 GEN-R01).
  const genUnits = groups.filter((g) => isGenerationUnit(g, join));
  const genWh = genUnits.reduce((s, g) => s + (unitMetricValue(g, 'energy_day_out') ?? 0), 0);
  if (genUnits.length && genWh > 0) {
    const { selfWh, soldWh, saved, sold } = solarValue(genWh, exportDayWh, dayRate?.buy ?? rate.buy, revDay, rate.sell);
    items.push(
      <Insight key="solar" tone="info" title={t('cards.solarTodayTitle', { energy: enWh(genWh) })}>
        {t('cards.solarTodaySelf', { energy: enWh(selfWh), money: saved != null ? `≈ ${fmt(saved)}` : '—' })}
        {soldWh > 0 ? ` ${t('cards.solarTodaySold', { energy: enWh(soldWh), money: sold != null ? fmt(sold) : '—' })}` : ''}
      </Insight>,
    );
  }
  if (floor.w != null && floor.w > 0.5) {
    items.push(
      <Insight key="base" tone="tip" title={t(k('cards.vampTitle'), { power: pwW(floor.w), energy: kwhText(floorKwh ?? 0) })}>
        {floorMoney != null ? `${t('cards.vampMoney', { money: fmt(floorMoney) })}${floorShare != null ? ` ${t('cards.vampShare', { pct: nf(floorShare) })}` : ''} ` : ''}
        {isNotableRise(floor.trendW, floor.w) ? `${t('cards.vampRise', { power: pwW(floor.trendW ?? 0) })} ` : ''}
        {t(k('cards.vampText'))}
        {controlledW && controlledNames.length ? ` ${t('cards.vampExcluding', { names: controlledNames.join(', ') })}` : ''}
        {huntButton}
      </Insight>,
    );
  } else if (floor.w == null && floor.nights > 0) {
    items.push(<Insight key="base" tone="info" title={t(k('cards.vampCollecting'), { n: floor.nights })}>{t(k('cards.vampCollectingText'))}{huntButton}</Insight>);
  } else if (huntButton) {
    // No night yet — the hunt works on live power and needs none (3φ rerun R07: nowhere to find it).
    items.push(<Insight key="base" tone="info" title={t('cards.vampHuntOnly')}>{t('cards.vampHuntOnlyText')}{huntButton}</Insight>);
  }

  return (
    <Card>
      <Stack spacing={1.5} sx={{ py: 0.5 }}>
        <Typography sx={{ fontWeight: 600, fontSize: 16 }}>{t('cards.savingsTitle')}</Typography>
        {items.length ? items : (
          <Typography sx={{ color: ds.inkMuted, fontSize: 14 }}>
            {t(lost ? 'cards.savingsLost' : 'cards.savingsEmpty')}
          </Typography>
        )}
        <Typography sx={{ fontSize: 11, color: ds.inkMuted }}>
          {money ? t('cards.savingsFootnote') : t('cards.savingsFootnoteEnergy')}
        </Typography>
      </Stack>
      {hunting && <VampireHuntDialog open onClose={() => setHunting(false)} join={join} weekRate={weekRate} fmt={fmt} />}
    </Card>
  );
};

const Insight: React.FC<{ tone: 'warn' | 'info' | 'tip' | 'good'; title: string; children: React.ReactNode }> = ({ tone, title, children }) => {
  const ds = useDsTokens();
  const color = tone === 'warn' ? ds.sevWarning : tone === 'good' ? ds.statusOnline : tone === 'tip' ? ds.solar : ds.primary;
  return (
    <Box sx={{ pl: 1.5, borderLeft: `3px solid ${color}` }}>
      <Typography sx={{ fontSize: 14, fontWeight: 600, color: ds.ink }}>{title}</Typography>
      <Box sx={{ fontSize: 13, color: ds.inkMuted }}>{children}</Box>
    </Box>
  );
};
