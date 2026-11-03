/**
 * HomeAssistantPanel — how RBgrid shows up in Home Assistant (rerun 16.10 HA-R02/R03: the hub
 * already publishes MQTT discovery for HA, the app said nothing about it; connecting RBgrid to HA
 * was ≈ 10 % first-try). The hub announces every point as an HA device with «Energy In / Out»
 * (total_increasing, Wh) plus «Cost / Revenue (Total)» for the site; this card says which of them
 * to pick in HA's Energy dashboard. Entity names are the hub's (English metric names), the device
 * names are the points' own names.
 */
import React from 'react';
import { Box, Stack, Typography, Table, TableBody, TableCell, TableHead, TableRow, Alert } from '@mui/material';
import { PanelCard } from '@rbgrid/design-system';
import { useConfigShadow } from '../../core';
import type { JsonObject } from '../../core';
import { useUnitsConfig } from '../../hooks/useUnitsConfig';
import { isGenerationRole } from '../../entities/siteModel';
import { t } from '../../i18n';
import { MqttBrokerForm } from './MqttBrokerForm';
import { fwAtLeast } from '../../entities/hubVersion';
import { useHubVersion } from '../../hooks/useHubVersion';
import { isDemo } from '../../services/connection';

/**
 * Firmware 1.1.2 names the export entity «Export value (Total)» (was «Revenue (Total)»; same uniq_id,
 * so HA keeps the entity) and fixes the money state_class HA refused (root 03.11). null = unknown.
 */
export const fixedMoneyEntities = (version: string | undefined | null): boolean | null => fwAtLeast(version, [1, 1, 2]);

export const HomeAssistantPanel: React.FC = () => {
  const mqtt = useConfigShadow('mqtt');
  const units = useConfigShadow('units');
  useUnitsConfig(); // pulls units-config if nobody has
  const fixed = fixedMoneyEntities(useHubVersion());
  React.useEffect(() => { mqtt.load().catch(() => undefined); /* eslint-disable-next-line react-hooks/exhaustive-deps */ }, []);
  const rep = mqtt.reported as Record<string, unknown>;
  const connected = rep.connected === true;
  const effective = typeof rep.effective_uri === 'string' && rep.effective_uri ? rep.effective_uri
    : typeof rep.broker_uri === 'string' && rep.broker_uri ? rep.broker_uri : null;
  const saved = rep.persisted === true || (typeof rep.broker_uri === 'string' && rep.broker_uri !== '');

  const list = Array.isArray(units.reported.units) ? (units.reported.units as JsonObject[]) : [];
  // The hub names the HA devices «RBGrid <point name>», the input «RBGrid Main» (rbjson.c discovery).
  const nameOf = (u: JsonObject) => (u.role === 'MAIN_SUPPLY' ? 'RBGrid Main' : `RBGrid ${String(u.name || '').trim() || hexOf(u)}`);
  const hexOf = (u: JsonObject) => (typeof u.unit_id === 'number' ? u.unit_id.toString(16).toUpperCase().padStart(4, '0') : '?');
  const input = list.find((u) => u.role === 'MAIN_SUPPLY' && u.type !== 'VOLTAGE_BUS');
  const gens = list.filter((u) => isGenerationRole(u.role as never));
  const loads = list.filter((u) => u !== input && !gens.includes(u) && u.type !== 'VOLTAGE_BUS' && u.role !== 'ALTERNATIVE_SUPPLY');

  const rows: { ha: string; pick: string }[] = [
    { ha: t('ha.gridConsumption'), pick: input ? `${nameOf(input)} · Energy In` : '—' },
    { ha: t('ha.returnToGrid'), pick: input ? `${nameOf(input)} · Energy Out` : '—' },
    { ha: t('ha.cost'), pick: 'Cost (Total)' },
    { ha: t('ha.compensation'), pick: fixed ? 'Export value (Total)' : 'Revenue (Total)' },
    ...gens.map((g) => ({ ha: t('ha.solar'), pick: `${nameOf(g)} · Energy Out` })),
    ...(loads.length ? [{ ha: t('ha.devices'), pick: loads.map((u) => `${nameOf(u)} · Energy In`).join(', ') }] : []),
  ];

  return (
    <Box id="home-assistant" sx={{ scrollMarginTop: 16 }}>
      <PanelCard title={t('ha.title')}>
        <Stack spacing={1.5}>
          <Typography variant="body2" color="text.secondary">{t('ha.intro')}</Typography>
          <Alert severity={connected ? 'success' : 'info'}>
            {connected
              ? (effective ? t('ha.connected', { uri: effective }) : t('ha.connectedDefault'))
              : t('ha.notConnected')}
            {connected && !saved ? ` ${t('ha.defaultBroker')}` : ''}
          </Alert>
          <MqttBrokerForm />
          <Typography variant="subtitle2" fontWeight={700}>{t('ha.mapTitle')}</Typography>
          <Box sx={{ overflowX: 'auto' }}>
            <Table size="small" aria-label={t('ha.mapTitle')}>
              <TableHead>
                <TableRow>
                  <TableCell>{t('ha.colHa')}</TableCell>
                  <TableCell>{t('ha.colPick')}</TableCell>
                </TableRow>
              </TableHead>
              <TableBody>
                {rows.map((r, i) => (
                  <TableRow key={i}>
                    <TableCell sx={{ whiteSpace: 'nowrap' }}>{r.ha}</TableCell>
                    <TableCell sx={{ fontFamily: 'monospace', fontSize: 13 }}>{r.pick}</TableCell>
                  </TableRow>
                ))}
              </TableBody>
            </Table>
          </Box>
          <Typography variant="caption" color="text.secondary">{t('ha.notDay')}</Typography>
          {!fixed && <Typography variant="caption" color="text.secondary">{t(isDemo() ? 'ha.oldHubDemo' : 'ha.oldHub')}</Typography>}
        </Stack>
      </PanelCard>
    </Box>
  );
};
