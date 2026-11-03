/**
 * HomeFlowCard — the Home overview's flow block with the «Схема | Сцена» choice (review 11 §7.1).
 * «Схема» (flat EnergyFlowHome) is the default; «Сцена» (isometric) is a per-viewer preference
 * kept in this browser and loaded lazily — the scene chunk is fetched only when picked.
 * The choice is offered only when there is something to draw (lines measured).
 *
 * Consumer groups (G5 `group_id`): when the device groups its lines, the schema gets tabs —
 * «Все» (each group folded into one card) and one tab per group (the group in the hub, its
 * members around it). A group card in «Все» opens its tab. The tab is a per-viewer view state.
 */
import React, { Suspense, lazy, useMemo, useState } from 'react';
import { Box, Stack, Tab, Tabs, ToggleButton, ToggleButtonGroup } from '@mui/material';
import { Card } from '@rbgrid/design-system';
import { EnergyFlowHome } from './EnergyFlowHome';
import { EnergyFlowPhases, EnergyFlowPhasesProps } from './EnergyFlowPhases';
import { FlowLegend } from './FlowLegend';
import { FlowFrame } from './flowFrame';
import { ALL_TAB, scopeFrame } from './flowGroups';
import { pw } from './flowHome';
import { t } from '../../i18n';

const EnergySceneHome = lazy(() => import(/* webpackChunkName: "scene-home" */ './EnergySceneHome'));

type View = 'schema' | 'scene' | 'phases';
const KEY = 'rbgrid.homeFlowView';
const TAB_KEY = 'rbgrid.homeFlowTab';
const read = (): View => { try { const v = localStorage.getItem(KEY); return v === 'scene' || v === 'phases' ? v : 'schema'; } catch { return 'schema'; } };
const readTab = (): string => { try { return localStorage.getItem(TAB_KEY) || ALL_TAB; } catch { return ALL_TAB; } };

/** `phases`: a multi-phase input's legs and lines — adds the «По фазам» view (operator 16.10). */
export const HomeFlowCard: React.FC<{ frame: FlowFrame; phases?: EnergyFlowPhasesProps | null }> = ({ frame, phases }) => {
  const [view, setView] = useState<View>(read);
  const [tabPref, setTabPref] = useState<string>(readTab);
  const canScene = frame.loads.length > 0;
  const pick = (v: View) => { setView(v); try { localStorage.setItem(KEY, v); } catch { /* per-viewer convenience only */ } };
  const showScene = canScene && view === 'scene';
  const canPhases = !!phases && phases.legs.length > 1;
  // Only the input and no generation: the schema draws nothing (the hero says it all) — a 3-phase
  // input then shows its phases straight away, else no card at all (.110 17.10: an empty card
  // holding just «Схема | По фазам»).
  const schemaEmpty = frame.loads.length === 0 && frame.solar == null;
  const showPhases = canPhases && (view === 'phases' || schemaEmpty);

  const groups = useMemo(() => frame.groups ?? [], [frame.groups]);
  // A remembered tab whose group is gone (regrouped on the device) falls back to «Все».
  const tab = groups.some((g) => g.id === tabPref) ? tabPref : ALL_TAB;
  const pickTab = (id: string) => { setTabPref(id); try { localStorage.setItem(TAB_KEY, id); } catch { /* per-viewer */ } };
  const scoped = useMemo(() => scopeFrame(frame, tab), [frame, tab]);
  const showTabs = !showScene && !showPhases && groups.length > 0;

  if (schemaEmpty && !canPhases) return null;
  return (
    <Card>
      {(canScene || (canPhases && !schemaEmpty) || showTabs) && (
        // Phones: the view toggle on top (right), the group tabs on their own full-width row.
        <Stack direction={{ xs: 'column-reverse', sm: 'row' }} alignItems={{ xs: 'stretch', sm: 'center' }} justifyContent="space-between" gap={1} sx={{ mb: 1, minWidth: 0 }}>
          {showTabs ? (
            <Tabs
              value={tab}
              onChange={(_, v: string) => pickTab(v)}
              variant="scrollable"
              scrollButtons="auto"
              allowScrollButtonsMobile
              aria-label={t('flow.tabsAria')}
              sx={{ minHeight: 36, minWidth: 0, '& .MuiTab-root': { minHeight: 36, py: 0.5, px: 1.5, textTransform: 'none', fontSize: 13 } }}
            >
              <Tab value={ALL_TAB} label={t('flow.tabAll')} />
              {groups.map((g) => {
                const v = scopeFrame(frame, g.id).house;
                return <Tab key={g.id} value={g.id} label={`${g.name} · ${pw(v)}`} />;
              })}
            </Tabs>
          ) : <Box />}
          {(canScene || canPhases) && (
            <ToggleButtonGroup size="small" exclusive value={view === 'phases' && !canPhases ? 'schema' : view} onChange={(_, v: View | null) => v && pick(v)} aria-label={t('scene.toggleAria')} sx={{ flexShrink: 0, alignSelf: { xs: 'flex-end', sm: 'center' } }}>
              <ToggleButton value="schema" sx={{ py: 0.25, px: 1.25, fontSize: 12 }}>{t('scene.viewSchema')}</ToggleButton>
              {canPhases && <ToggleButton value="phases" sx={{ py: 0.25, px: 1.25, fontSize: 12 }}>{t('flow.viewPhases')}</ToggleButton>}
              {canScene && <ToggleButton value="scene" sx={{ py: 0.25, px: 1.25, fontSize: 12 }}>{t('scene.viewScene')}</ToggleButton>}
            </ToggleButtonGroup>
          )}
        </Stack>
      )}
      {showPhases && phases ? (
        <EnergyFlowPhases {...phases} />
      ) : showScene ? (
        <Suspense fallback={<Box sx={{ minHeight: 240 }} />}>
          <EnergySceneHome frame={frame} />
        </Suspense>
      ) : (
        <>
          <EnergyFlowHome frame={scoped} onOpenGroup={pickTab} />
          {scoped.loads.length > 0 && (
            <FlowLegend
              grid={!scoped.scope}
              exportable={!scoped.scope && scoped.gridHasDirection && (scoped.solar != null || scoped.grid < 0)}
              solar={!scoped.scope && scoped.solar != null}
              noData={scoped.loads.some((l) => l.v == null)}
              groups={scoped.loads.some((l) => l.group)}
            />
          )}
        </>
      )}
    </Card>
  );
};
