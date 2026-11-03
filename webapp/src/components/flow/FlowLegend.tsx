/**
 * FlowLegend — the colour key under the flow schemas (Дом / дерево / анализ): which colour is
 * the grid, export, generation, consumers, no-data, a consumer group. Only what the schema
 * actually draws is listed (no solar entry without generation — spec §10.3).
 */
import React from 'react';
import { useDsTokens } from '../../theme/useDsTokens';
import { ChartLegend, LegendItem } from '../common/ChartLegend';
import { t } from '../../i18n';

export interface FlowLegendProps {
  grid?: boolean;
  exportable?: boolean;
  solar?: boolean;
  loads?: boolean;
  noData?: boolean;
  groups?: boolean;
}

export const FlowLegend: React.FC<FlowLegendProps> = ({ grid = true, exportable, solar, loads = true, noData, groups }) => {
  const ds = useDsTokens();
  const items: LegendItem[] = [];
  if (grid) items.push({ label: t('legend.gridImport'), color: ds.flowImport, mark: 'line' });
  if (exportable) items.push({ label: t('legend.gridExport'), color: ds.flowExport, mark: 'line' });
  if (solar) items.push({ label: t('legend.solar'), color: ds.solar, mark: 'line' });
  if (loads) items.push({ label: t('legend.loads'), color: ds.primary, mark: 'line' });
  if (groups) items.push({ label: t('legend.group'), color: ds.primary, mark: 'box' });
  if (noData) items.push({ label: t('legend.noData'), color: ds.statusOffline, mark: 'dash' });
  return <ChartLegend items={items} sx={{ mt: 1.5, justifyContent: 'center' }} />;
};
