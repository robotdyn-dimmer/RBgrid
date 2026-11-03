/**
 * MoneyLens — the «Мощность ⇄ Деньги» lens as a React context + value components.
 *
 *   <MoneyLensProvider mode rate fmt> … <PowerValue w={signedW} /> … <EnergyValue wh={…} money={…} />
 *
 * The lens changes which figure LEADS and which becomes the small secondary line; it never
 * hides the physical value. Money is in the device currency (useMoneyFormat), estimated from
 * the live rate (system.tariff, E1) and marked «≈»; export earns at the sell rate.
 * Every Overview card follows the lens EXCEPT the flow schemas (they draw physical flows, kW).
 * Physical limits keep their unit inside a card: the limit bar, headroom, phase currents and
 * power quality stay kW/A/V/Hz; only the figure that leads switches.
 */
import React, { createContext, useContext } from 'react';
import { Box, Typography } from '@mui/material';
import { useDsTokens } from '../theme/useDsTokens';
import { pwW, enWh } from '../components/common/ruFormat';
import { DisplayMode } from '../hooks/useDisplayMode';
import { LiveRate, DayRate, moneyPerHour, moneyForWh, generationPerHour } from './costRate';
import { t } from '../i18n';

export interface MoneyLens {
  mode: DisplayMode;
  rate: LiveRate;
  /** The input's signed power (W, − export) — values generation by what it displaces. */
  gridW?: number | null;
  /** Formats a sum in the device currency (useMoneyFormat().money). */
  fmt: (v: number | null | undefined) => string;
  /** Today's average prices at the input — day amounts (kWh today) are priced with these,
   *  never with the current zone (dayAvgRate; the live rate is the fallback). */
  dayRate?: DayRate;
  /** Today's generation and its value (solarValue) — a generation line's day money is its
   *  share of this, the same figure as «Солнце сегодня». */
  solarDay?: { wh: number; money: number | null };
}

const Ctx = createContext<MoneyLens>({
  mode: 'power',
  rate: { buy: null, sell: null, zone: null, currency: null, netPerHour: null },
  fmt: (v) => (v == null ? '—' : String(v)),
});

export const MoneyLensProvider: React.FC<MoneyLens & { children: React.ReactNode }> = ({ children, ...lens }) => (
  <Ctx.Provider value={lens}>{children}</Ctx.Provider>
);

export const useMoneyLens = (): MoneyLens => useContext(Ctx);

/** «≈ 11,90 ₽/ч» for a signed power; «+…» when earning; '—' when the rate is unknown. */
export function useMoneyPerHourText(signedW: number | null | undefined, generation = false): { text: string; earning: boolean } {
  const { rate, fmt, gridW } = useMoneyLens();
  const m = generation ? generationPerHour(signedW, gridW, rate) : moneyPerHour(signedW, rate);
  if (m == null) return { text: '—', earning: false };
  const earning = m < 0;
  return { text: `${t('common.approx')} ${earning ? '+' : ''}${fmt(Math.abs(m))}${t('units.perHour')}`, earning };
}

type Size = 'hero' | 'metric' | 'body';
const SIZE: Record<Size, { main: object; sub: number }> = {
  hero: { main: { fontSize: { xs: 44, md: 52 }, lineHeight: 1.08, fontWeight: 600, letterSpacing: '-0.02em' }, sub: 14 },
  metric: { main: { fontSize: { xs: 18, md: 22 }, lineHeight: 1.15, fontWeight: 600 }, sub: 12 },
  body: { main: { fontSize: 15, fontWeight: 600 }, sub: 12 },
};

/** A live power figure under the lens (signed W: + consumption, − export). */
export const PowerValue: React.FC<{ w: number | null | undefined; size?: Size; prefix?: string; color?: string; generation?: boolean }> = ({
  w, size = 'metric', prefix = '', color, generation = false,
}) => {
  const ds = useDsTokens();
  const { mode } = useMoneyLens();
  const money = useMoneyPerHourText(w, generation);
  const power = w == null ? '—' : `${prefix}${pwW(Math.abs(w))}`;
  const moneyLeads = mode === 'money' && money.text !== '—';
  const mainText = moneyLeads ? money.text : power;
  const subText = moneyLeads ? power : money.text !== '—' ? money.text : null;
  const mainColor = moneyLeads && money.earning ? ds.flowExport : color ?? ds.ink;
  return (
    <Box sx={{ minWidth: 0 }}>
      <Typography sx={{ ...SIZE[size].main, color: mainColor, whiteSpace: 'nowrap', fontVariantNumeric: 'tabular-nums' }}>
        {mainText}
      </Typography>
      {subText && (
        <Typography sx={{ fontSize: SIZE[size].sub, color: ds.inkMuted, fontVariantNumeric: 'tabular-nums', whiteSpace: 'nowrap' }}>
          {subText}
        </Typography>
      )}
    </Box>
  );
};

/**
 * An energy amount under the lens. Pass the device's own money when it has it (cost_day etc.);
 * otherwise money is estimated at the current rate and marked «≈».
 */
export const EnergyValue: React.FC<{ wh: number | null | undefined; money?: number | null; size?: Size; color?: string }> = ({
  wh, money, size = 'metric', color,
}) => {
  const ds = useDsTokens();
  const { mode, rate, fmt, dayRate } = useMoneyLens();
  const exact = money != null;
  const m = exact ? money : wh != null && dayRate?.buy != null ? (wh / 1000) * dayRate.buy : moneyForWh(wh, rate);
  const moneyText = m == null ? null : `${exact ? '' : `${t('common.approx')} `}${fmt(m)}`;
  const energyText = enWh(wh);
  const moneyLeads = mode === 'money' && moneyText != null;
  return (
    <Box sx={{ minWidth: 0 }}>
      <Typography sx={{ ...SIZE[size].main, color: color ?? ds.ink, whiteSpace: 'nowrap', fontVariantNumeric: 'tabular-nums' }}>
        {moneyLeads ? moneyText : energyText}
      </Typography>
      {(moneyLeads ? energyText : moneyText) && (
        <Typography sx={{ fontSize: SIZE[size].sub, color: ds.inkMuted, whiteSpace: 'nowrap', fontVariantNumeric: 'tabular-nums' }}>
          {moneyLeads ? energyText : moneyText}
        </Typography>
      )}
    </Box>
  );
};

export interface LensText {
  /** The figure that leads under the active lens. */
  main: string;
  /** The counterpart in the other unit (small line / tooltip), or null when unknown. */
  sub: string | null;
  /** True when `main` is money. */
  money: boolean;
  /** Money that earns (export at the sell rate). */
  earning: boolean;
}

/**
 * Plain-string lens formatters for tables, KPI tiles and SVG labels (where the
 * PowerValue/EnergyValue components don't fit). Same rules: money only with a known rate,
 * marked «≈» when estimated; the physical value never disappears — it becomes the `sub`.
 */
export function useLensFormat() {
  return lensFormat(useMoneyLens());
}

/** Pure form of {@link useLensFormat} — for the page that OWNS the provider (it can't read it). */
export function lensFormat({ mode, rate, fmt, gridW, dayRate, solarDay }: MoneyLens) {
  const moneyMode = mode === 'money';
  const approx = t('common.approx');
  /** Signed W → «≈ 38,40 ₽/ч» / «≈ +0,88 ₽/ч» (earning) or null. */
  const perHour = (w: number | null | undefined): { text: string; earning: boolean } | null => {
    const m = moneyPerHour(w, rate);
    if (m == null) return null;
    return { text: `${approx} ${m < 0 ? '+' : ''}${fmt(Math.abs(m))}${t('units.perHour')}`, earning: m < 0 };
  };
  /** Wh → money: taken at the buy rate, given (out) at the sell rate; `exact` = the device's own sum. */
  const energyMoney = (wh: number | null | undefined, out = false, exact?: number | null, estimate = false): string | null => {
    const sign = out ? '+' : '';
    if (exact != null) return `${estimate ? `${approx} ` : ''}${sign}${fmt(exact)}`;
    if (wh == null) return null;
    const r = (out ? dayRate?.sell : dayRate?.buy) ?? (out ? rate.sell : rate.buy);
    return r == null ? null : `${approx} ${sign}${fmt((wh / 1000) * r)}`;
  };
  const power = (w: number | null | undefined, signed = true, generation = false): LensText => {
    const phys = w == null ? '—' : `${signed && w < 0 ? '↑ ' : ''}${pwW(Math.abs(w))}`;
    // Generation is worth what it displaces (buy price) + what it sells (GEN-05), like the tiles.
    const gm = generation && w != null ? generationPerHour(w, gridW, rate) : null;
    const m = w == null ? null : generation && gm != null
      ? { text: `${approx} ${gm < 0 ? '+' : ''}${fmt(Math.abs(gm))}${t('units.perHour')}`, earning: gm < 0 }
      : perHour(w);
    const lead = moneyMode && m != null;
    return { main: lead ? (m as { text: string }).text : phys, sub: lead ? phys : m?.text ?? null, money: lead, earning: !!m?.earning };
  };
  const energy = (wh: number | null | undefined, out = false, exact?: number | null, estimate = false): LensText => {
    const phys = `${out && wh != null ? '↑ ' : ''}${enWh(wh ?? null)}`;
    const m = energyMoney(wh, out, exact, estimate);
    const lead = moneyMode && m != null;
    return { main: lead ? (m as string) : phys, sub: lead ? phys : m, money: lead, earning: lead && out };
  };
  /** A generation line's day money = its share of the sun's value today (null when unknown). */
  const genMoney = (wh: number | null | undefined): number | null =>
    wh != null && solarDay && solarDay.money != null && solarDay.wh > 0 ? solarDay.money * (wh / solarDay.wh) : null;
  return { moneyMode, power, energy, perHour, energyMoney, genMoney, rate, fmt, dayRate };
}
