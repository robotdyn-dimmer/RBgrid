/**
 * KpiRow — the KPI×5 strip used by the Станции and Аналитика overviews (spec §4.3.1 / §4.4.1).
 * Responsive per §9: 2 on phones, 3 at 820–1100, up to 5 from 1100. Values are Sans tabular
 * figures (mono is reserved for tech details/slugs/time — review 03 P1-10/15).
 */
import React from 'react';
import { Box, Stack, Typography } from '@mui/material';
import { Card } from '@rbgrid/design-system';
import { useDsTokens } from '../../theme/useDsTokens';

export interface KpiItem {
  label: string;
  value: string;
  sub?: string;
  /** Token colour for the value (e.g. a warning tint); defaults to primary ink. */
  valueColor?: string;
  subColor?: string;
}

export const KpiRow: React.FC<{ items: KpiItem[] }> = ({ items }) => {
  const ds = useDsTokens();
  return (
    <Box
      sx={{
        display: 'grid',
        gap: 2,
        gridTemplateColumns: {
          xs: 'repeat(2, minmax(0, 1fr))',
          md: 'repeat(3, minmax(0, 1fr))',
          lg: `repeat(${Math.min(items.length, 5)}, minmax(0, 1fr))`,
        },
      }}
    >
      {items.map((k, i) => (
        <Card key={i}>
          <Stack spacing={0.5} sx={{ py: 0.25 }}>
            <Typography sx={{ fontSize: 12, letterSpacing: '0.02em', color: ds.inkMuted }}>{k.label}</Typography>
            <Typography sx={{ fontVariantNumeric: 'tabular-nums', fontSize: { xs: 18, md: 22 }, fontWeight: 600, color: k.valueColor ?? ds.ink, lineHeight: 1.15, overflowWrap: 'anywhere' }}>
              {k.value}
            </Typography>
            {k.sub && <Typography sx={{ fontSize: 12, color: k.subColor ?? ds.inkMuted }}>{k.sub}</Typography>}
          </Stack>
        </Card>
      ))}
    </Box>
  );
};
