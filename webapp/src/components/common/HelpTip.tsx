/**
 * HelpTip — a small (i) next to a term (UX audit W3-2 / T14): one tap opens a short explanation in
 * plain words; «Подробнее» links to the documentation page when one is known for the term
 * (DOC_PAGES — pages owned by the site / content team; a term without a page shows text only).
 * Keyboard: the button is focusable, Esc closes.
 */
import React, { useState } from 'react';
import { IconButton, Popover, Typography, Link, Box } from '@mui/material';
import InfoOutlinedIcon from '@mui/icons-material/InfoOutlined';
import { t } from '../../i18n';

/** Documentation page per help topic (content:rbgrid, 2026-10-16 — /webapp-* slugs are kept 1:1
 *  through the site migration; 10/10 topics). */
const TARIFF_DOC = 'https://www.rbgrid.io/webapp-tariff-configuration';
const UNITS_DOC = 'https://www.rbgrid.io/webapp-units-configuration';
export const DOC_PAGES: Partial<Record<HelpTopic, string>> = {
  netMetering: TARIFF_DOC, sellRatio: TARIFF_DOC, zoneRatio: TARIFF_DOC, defaultZone: TARIFF_DOC, zoneDays: TARIFF_DOC,
  role: UNITS_DOC, partial: UNITS_DOC, closedRecords: UNITS_DOC, phase: UNITS_DOC, notBought: TARIFF_DOC,
};

export type HelpTopic =
  | 'netMetering' | 'sellRatio' | 'zoneRatio' | 'defaultZone' | 'zoneDays'
  | 'role' | 'phase' | 'partial' | 'notBought' | 'closedRecords';

export const HelpTip: React.FC<{ topic: HelpTopic; label?: string }> = ({ topic, label }) => {
  const [anchor, setAnchor] = useState<HTMLElement | null>(null);
  const url = DOC_PAGES[topic];
  return (
    <>
      <IconButton size="small" aria-label={t('help.what', { what: label ?? t(`help.${topic}Title`) })}
        aria-haspopup="dialog" aria-expanded={!!anchor}
        onClick={(e) => { e.stopPropagation(); setAnchor(e.currentTarget); }}
        // A finger needs 44 px (HEU-R06: the (i) was 20 × 20 on a phone); a mouse keeps it compact.
        sx={{ p: 0.25, ml: 0.25, color: 'text.secondary', verticalAlign: 'middle',
          '@media (pointer: coarse)': { minWidth: 44, minHeight: 44, m: -1, ml: 0 } }}>
        <InfoOutlinedIcon sx={{ fontSize: 16 }} />
      </IconButton>
      <Popover open={!!anchor} anchorEl={anchor} onClose={() => setAnchor(null)}
        anchorOrigin={{ vertical: 'bottom', horizontal: 'left' }} transformOrigin={{ vertical: 'top', horizontal: 'left' }}>
        <Box sx={{ p: 1.5, maxWidth: 320 }}>
          <Typography variant="subtitle2" fontWeight={700} sx={{ mb: 0.5 }}>{t(`help.${topic}Title`)}</Typography>
          <Typography variant="body2" color="text.secondary">{t(`help.${topic}`)}</Typography>
          {url && (
            <Link href={url} target="_blank" rel="noopener noreferrer" variant="body2" sx={{ display: 'inline-block', mt: 1 }}>
              {t('help.more')}
            </Link>
          )}
        </Box>
      </Popover>
    </>
  );
};
