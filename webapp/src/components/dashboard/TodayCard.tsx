/**
 * TodayCard — the «Сегодня» card of the Home overview (spec §4.1): what was taken from the grid
 * today, what was given back (prosumer) and the day's net — every figure under the
 * «Мощность ⇄ Деньги» lens (energy or money leads, the other is the small line). It replaced
 * the separate «Стоимость сегодня» card, which in money mode repeated the same sum.
 * The live tariff (zone · rate) is reference info and reads the same in both modes.
 *
 * When the day isn't established yet (time not synced, tq=0) → «—», never a misleading 0
 * (spec §5.2 / §4.2). ru-RU figures, tabular (global §2.1), «—» for null.
 */
import React from 'react';
import { Link, Stack, Typography } from '@mui/material';
import { Card } from '@rbgrid/design-system';
import { useDsTokens } from '../../theme/useDsTokens';
import { useLensFormat, LensText } from '../../economics/MoneyLens';
import { t } from '../../i18n';

export interface TodayCardProps {
  energyInWh: number | null;
  energyOutWh: number | null;
  timeSynced: boolean;
  /** The device's own money for today (cost of import / revenue of export) — exact, not «≈». */
  costDay?: number | null;
  revDay?: number | null;
  /** Current tariff zone, human label (null = no active plan). */
  zoneName?: string | null;
  /** Current buy rate, already formatted. */
  rateText?: string | null;
  /** The hub counts by its factory example plan — say so and offer to set one's own (W191-06). */
  placeholderTariff?: boolean;
  /** OSS build — no tariff engine. */
  oss?: boolean;
  /** Month to date (closed days + today), the same figure as Economics «месяц» (HOME-21). */
  month?: { wh: number; cost: number | null } | null;
}

const Figure: React.FC<{ label: string; v: LensText; color?: string }> = ({ label, v, color }) => {
  const ds = useDsTokens();
  return (
    <Stack spacing={0.25} sx={{ minWidth: 0 }}>
      <Typography sx={{ fontSize: 12, letterSpacing: '0.02em' }} color="text.secondary">{label}</Typography>
      <Typography sx={{ fontSize: { xs: 18, md: 22 }, lineHeight: 1.15, fontWeight: 600, color: color ?? ds.ink, whiteSpace: 'nowrap', fontVariantNumeric: 'tabular-nums' }}>
        {v.main}
      </Typography>
      {v.sub && (
        <Typography sx={{ fontSize: 12, color: ds.inkMuted, whiteSpace: 'nowrap', fontVariantNumeric: 'tabular-nums' }}>{v.sub}</Typography>
      )}
    </Stack>
  );
};

export const TodayCard: React.FC<TodayCardProps> = ({
  energyInWh, energyOutWh, timeSynced, costDay, revDay, zoneName, rateText, oss, month, placeholderTariff }) => {
  const ds = useDsTokens();
  const lens = useLensFormat();
  // < 10 Wh is noise, not «Отдано +0,00 € · 0,0 кВт·ч».
  const showOut = energyOutWh != null && energyOutWh >= 10;
  const took = lens.energy(energyInWh, false, costDay);
  const gave = lens.energy(energyOutWh, true, revDay);
  // Net of the day: money = cost − revenue (exact device sums when both exist), energy = in − out.
  let net: LensText | null = null;
  if (showOut && energyInWh != null) {
    const netWh = energyInWh - (energyOutWh as number);
    // Exact when the cost is exact (no revenue = 0); else the lens estimates — never an estimate
    // that contradicts the exact «Взято» next to it.
    const netMoney = costDay != null ? costDay - (revDay ?? 0) : null;
    net = lens.energy(netWh, false, netMoney);
  }
  return (
    <Card>
      <Stack spacing={1.5} sx={{ py: 0.5 }}>
        <Typography sx={{ fontSize: 12, letterSpacing: '0.02em' }} color="text.secondary">{t('cards.todayTitle')}</Typography>
        {/* One rule for «today» with the hub clock unset (3φ rerun R01: «—» here, figures on six other
            screens): the hub's day counter everywhere, said honestly where its day starts. */}
        <Stack direction="row" columnGap={4} rowGap={1.5} flexWrap="wrap" useFlexGap>
          <Figure label={t('cards.todayImported')} v={took} />
          {showOut && <Figure label={t('cards.todayExported')} v={gave} color={ds.flowExport} />}
          {net && <Figure label={t('cards.todayNet')} v={net} />}
        </Stack>
        {!timeSynced && (
          <Typography sx={{ fontSize: 12, color: ds.sevWarning }}>{t('cards.todayClockNotSet')}</Typography>
        )}
        {month && month.wh > 0 && (
          <Stack direction="row" spacing={1} alignItems="baseline" sx={{ pt: 0.5, borderTop: 1, borderColor: 'divider' }}>
            <Typography sx={{ fontSize: 12 }} color="text.secondary">{t('cards.monthToDate')}</Typography>
            <Typography sx={{ fontSize: 15, fontWeight: 600, fontVariantNumeric: 'tabular-nums' }}>
              {(() => { const v = lens.energy(month.wh, false, month.cost); return v.sub ? `${v.main} · ${v.sub}` : v.main; })()}
            </Typography>
          </Stack>
        )}
        {oss ? (
          <Typography sx={{ fontSize: 12 }} color="text.secondary">{t('cards.costNoTariff')}</Typography>
        ) : zoneName ? (
          <Typography sx={{ fontSize: 12 }} color="text.secondary">
            {t('cards.costZone')} <span style={{ color: ds.ink }}>{zoneName}</span>
            {rateText ? ` · ${rateText}` : ''}
          </Typography>
        ) : null}
        {!oss && placeholderTariff && (
          <Typography sx={{ fontSize: 12, color: ds.sevWarning }}>
            {t('cards.placeholderTariff')}{' '}
            <Link href="#/onboarding" underline="hover" sx={{ fontSize: 12 }}>{t('cards.setOwnTariff')}</Link>
          </Typography>
        )}
      </Stack>
    </Card>
  );
};
