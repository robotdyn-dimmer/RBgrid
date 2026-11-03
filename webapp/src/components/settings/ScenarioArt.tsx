/**
 * ScenarioArt — small STATIC isometric illustrations for the scenario cards (review 11 §7.1:
 * «Мастер · шаг Объект»: дом / цех+щит / паркинг; no animation, no numbers). Decorative —
 * the card text names the scenario, so the SVG is aria-hidden. Same iso helpers and theme
 * tokens as the home scene; only the role accent is coloured.
 */
import React, { useMemo } from 'react';
import { Box } from '@mui/material';
import { iso, ISO_TOKENS } from '../flow/iso';
import { useTheme } from '../../theme/ThemeProvider';
import { useDsTokens } from '../../theme/useDsTokens';
import type { SiteProfile } from '../../types/config';

function art(kind: SiteProfile): string {
  const I = iso(80, 64);
  let s = I.poly([[-60, -60, 0], [60, -60, 0], [60, 60, 0], [-60, 60, 0]], 'var(--iso-ground)', ' stroke="var(--iso-ground-line)" stroke-width="1"');
  if (kind === 'home') {
    // house with a gable roof + a small switchboard
    s += I.box(-30, -24, 0, 50, 42, 26);
    s += I.poly([[-30, -24, 26], [20, -24, 26], [20, -3, 40], [-30, -3, 40]], 'var(--iso-top)');
    s += I.poly([[20, -24, 26], [20, 18, 26], [20, -3, 40]], 'var(--iso-right)');
    s += I.poly([[-30, 18, 26], [20, 18, 26], [20, -3, 40], [-30, -3, 40]], 'var(--iso-left)');
    s += I.poly([[-8, 18, 10], [6, 18, 10], [6, 18, 19], [-8, 18, 19]], 'var(--primary)', ' fill-opacity=".45"');
    s += I.box(30, 10, 0, 7, 12, 20, { top: 'var(--primary)' });
  } else if (kind === 'industrial') {
    // workshop with a saw-tooth roof + main switchboard row
    s += I.box(-40, -30, 0, 60, 50, 24);
    for (let k = 0; k < 3; k++) {
      const x0 = -40 + k * 20;
      s += I.poly([[x0, -30, 24], [x0 + 20, -30, 24], [x0 + 20, 20, 24], [x0, 20, 24]], 'var(--iso-top)');
      s += I.poly([[x0 + 20, -30, 24], [x0 + 20, 20, 24], [x0 + 20, 20, 34], [x0 + 20, -30, 34]], 'var(--primary)', ' fill-opacity=".35"');
    }
    for (let k = 0; k < 3; k++) s += I.box(30 + k * 0, -20 + k * 14, 0, 8, 12, 22, { top: k === 0 ? 'var(--primary)' : 'var(--iso-top)' });
  } else {
    // parking: three charger posts with cars
    for (let k = 0; k < 3; k++) {
      const y = -40 + k * 30;
      s += I.box(-36, y, 0, 6, 6, 22, { top: 'var(--primary)' });
      s += I.box(-22, y - 6, 0, 32, 18, 8) + I.box(-18, y - 3, 8, 20, 12, 7);
      s += '<path d="' + I.path([[-30, y + 3, 6], [-22, y + 3, 4]]) + '" stroke="var(--primary)" stroke-width="1.6" fill="none"/>';
    }
  }
  return s;
}

export const ScenarioArt: React.FC<{ kind: SiteProfile; height?: number }> = ({ kind, height = 84 }) => {
  const { isDarkMode } = useTheme();
  const ds = useDsTokens();
  const tok = isDarkMode ? ISO_TOKENS.dark : ISO_TOKENS.light;
  const markup = useMemo(() => art(kind), [kind]);
  return (
    <Box
      aria-hidden
      sx={{
        '--iso-top': tok.top, '--iso-left': tok.left, '--iso-right': tok.right, '--iso-edge': tok.edge,
        '--iso-ground': tok.ground, '--iso-ground-line': tok.groundLine, '--primary': ds.primary,
        height, mb: 1, '& svg': { display: 'block', height: '100%', width: 'auto', mx: 'auto' },
      } as object}
      dangerouslySetInnerHTML={{ __html: '<svg viewBox="0 0 160 110" focusable="false">' + markup + '</svg>' }}
    />
  );
};
