/**
 * useMoneyFormat — money/rate formatters bound to the DEVICE's currency and precision.
 * Source of truth: schema `money{currency_code, money_decimals, rate_decimals}` (canon
 * fc0f962b); the live telemetry `system.tariff.currency` wins for the code when present
 * (it is the ACTIVE plan's currency). Nothing about currency is hardcoded in the UI.
 */
import { useMemo } from 'react';
import { useCapabilities } from '../core';
import { money, rate } from '../components/common/ruFormat';

export interface MoneyFormatter {
  currencyCode: string | null;
  money: (v: number | null | undefined) => string;
  rate: (v: number | null | undefined) => string;
}

export function useMoneyFormat(liveCurrency?: string | null): MoneyFormatter {
  const caps = useCapabilities();
  const { currencyCode: schemaCode, moneyDecimals, rateDecimals } = caps.money;
  const code = liveCurrency && /^[A-Z]{3}$/.test(liveCurrency) ? liveCurrency : schemaCode;
  return useMemo(
    () => ({
      currencyCode: code,
      money: (v) => money(v, code, moneyDecimals),
      rate: (v) => rate(v, code, rateDecimals),
    }),
    [code, moneyDecimals, rateDecimals],
  );
}
