/**
 * GridPanel — the site's mains system (root delta №4, canon bbf06397): system-config
 * `grid.system` = "" (not set = 1P, as the hub counts it) | 1P | SPLIT (US 120/240 V) | 3P. Applies
 * live, persisted. The nominals (V L-N, Hz) are hardware-config `network.*`, edited here (the only
 * part of hardware-config the product uses — no built-in ADC, operator 18.10) and applied on reboot.
 * Warns about points whose phase isn't valid on the chosen system (contract client rule:
 * 1P {0,1} · SPLIT {0,1,2,5} · 3P {0..7}). Shown only when the hub reports `grid`.
 * A system the hub does not list in `enums.grid_system_validated` carries «не проверено на
 * железе» IN its option label (F20: a screenshot of the list must not read as a claim).
 */
import React, { useMemo, useState } from 'react';
import { Alert, Box, MenuItem, Stack, TextField, Typography } from '@mui/material';
import { PanelCard } from '@rbgrid/design-system';
import { useCapabilities, useConfigShadow } from '../../core';
import type { JsonObject } from '../../core';
import { useUnitsConfig, phaseLabel } from '../../hooks/useUnitsConfig';
import { useGridSystem } from '../../hooks/useGridSystem';
import { GridKind, phaseChoices, mainsVoltageText, deriveGridKind, validatedKinds } from '../../entities/gridSystem';
import { nf } from '../common/ruFormat';
import { t } from '../../i18n';
import { useTelemetryEntities } from '../../hooks/useTelemetryEntities';
import { mainUnit } from '../../pages/overviewData';
import { savedText } from '../../services/savedText';
import { RELEASE_HIDDEN } from '../../constants/release';
import { ShadowConfigEditor } from './ShadowConfigEditor';

const KINDS: GridKind[] = ['1P', 'SPLIT', '3P'];

export const GridPanel: React.FC = () => {
  const caps = useCapabilities();
  const join = useUnitsConfig();
  const grid = useGridSystem(join);
  const validated = validatedKinds(caps.stringEnums.grid_system_validated);
  const kindLabel = (k: GridKind) => `${t(`gridPanel.kind${k}`)}${validated.includes(k) ? '' : t('grid.notValidated')}`;
  // What the points' phases suggest when nothing is set (the hub then counts 1P).
  // The input itself measuring three phases (U3I3 ph[]) suggests 3P before any line does (3φ rerun
  // R04: «not set — counted as single-phase» while every other screen showed L1–L3).
  const { data: telem } = useTelemetryEntities();
  const legs = telem ? mainUnit(telem.units, join)?.phases?.length ?? 0 : 0;
  const suggested = legs === 3 ? '3P' : deriveGridKind({ phases: join.phases, inputPhase: join.input.phase });
  const sys = useConfigShadow('system');
  const units = useConfigShadow('units');
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState<{ sev: 'success' | 'error'; text: string } | null>(null);

  const declared = (() => {
    const g = sys.reported.grid as { system?: unknown } | undefined;
    return typeof g?.system === 'string' ? g.system.toUpperCase() : '';
  })();
  const kinds = (caps.stringEnums.grid_system?.length ? caps.stringEnums.grid_system : KINDS)
    .map((k) => k.toUpperCase()).filter((k): k is GridKind => (KINDS as string[]).includes(k))
    // Split phase is deferred from v1.1.0: not offered, unless this hub is already set to it.
    .filter((k) => k !== 'SPLIT' || !RELEASE_HIDDEN.splitPhase || declared === 'SPLIT');

  // Points whose phase the CHOSEN system doesn't allow. Nothing chosen → no warning (the hub
  // counts 1P, the points' phases only suggest — rerun 16.10 INP-R11/PNT-R05).
  const misfit = useMemo(() => {
    if (!declared) return [];
    const ok = new Set(phaseChoices(grid.kind));
    const list = Array.isArray(units.reported.units) ? (units.reported.units as JsonObject[]) : [];
    return list
      .filter((u) => typeof u.phase === 'number' && !ok.has(u.phase))
      .map((u) => `${String(u.name || u.short_name || '')} (${phaseLabel(u.phase as number)})`);
  }, [units.reported, grid.kind, declared]);

  if (!grid.supported) return null;

  const choose = async (v: string) => {
    setBusy(true); setMsg(null);
    try {
      await grid.save(v as GridKind | '');
      setMsg({ sev: 'success', text: savedText('gridPanel.saved') });
    } catch (e) {
      setMsg({ sev: 'error', text: t('gridPanel.error', { reason: e instanceof Error ? e.message : String(e) }) });
    } finally { setBusy(false); }
  };

  return (
    <Box id="grid" sx={{ scrollMarginTop: 16 }}>
      <PanelCard title={t('gridPanel.title')}>
        <Stack spacing={1.5}>
          <Typography variant="body2" color="text.secondary">{t('gridPanel.intro')}</Typography>
          <TextField
            select size="small" label={t('gridPanel.system')} value={declared} disabled={busy}
            onChange={(e) => { void choose(e.target.value); }}
            // A declared system not yet validated says what that means (3φ audit H3P-08: owners read
            // «not validated on hardware» as «my readings are wrong»).
            helperText={declared
              ? (validated.includes(declared as GridKind) ? undefined : t('gridPanel.notValidatedHelp'))
              : suggested === '1P' ? undefined : t('gridPanel.suggest', { kind: kindLabel(suggested) })}
            sx={{ maxWidth: 360 }} SelectProps={{ displayEmpty: true }} InputLabelProps={{ shrink: true }}
          >
            <MenuItem value="">{t('gridPanel.auto')}</MenuItem>
            {kinds.map((k) => <MenuItem key={k} value={k}>{kindLabel(k)}</MenuItem>)}
          </TextField>
          <Typography variant="body2">
            {t('gridPanel.nominal', { v: mainsVoltageText(grid), hz: nf(grid.nominalHz) })}
          </Typography>
          <ShadowConfigEditor domain="hardware" only={['network']} />
          {misfit.length > 0 && <Alert severity="warning">{t('gridPanel.misfit', { points: misfit.join(', ') })}</Alert>}
          {msg && <Alert severity={msg.sev}>{msg.text}</Alert>}
        </Stack>
    </PanelCard>
    </Box>
  );
};
