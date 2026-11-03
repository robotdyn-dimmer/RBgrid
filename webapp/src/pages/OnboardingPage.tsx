/**
 * OnboardingPage — the first-run setup wizard (spec §4.1 / §7 «Мастер»), design-system pass.
 *
 * The step model, completion logic and navigation all stay in useOnboarding (untouched here):
 * this file only owns the VISUALS — an 8-step tokenised Stepper (skippable steps carry a
 * «можно пропустить» caption), Russian labels/bodies, design-system cards, and a final step
 * that shows a summary + reboot-with-progress. Per-step work still reuses the existing panels
 * (Security, shadow config editors, Sensors/bind, Nodes).
 */
import { SiteProfilePanel } from '../components/settings/SiteProfilePanel';
import { channelLabel } from '../entities/phaseGroup';
import React, { useState } from 'react';
import {
  Box, Typography, Stepper, Step, StepButton, Button, Stack, Alert, CircularProgress, Chip,
  Divider, LinearProgress, useMediaQuery, useTheme,
} from '@mui/material';
import { useNavigate } from 'react-router-dom';
import { Card, StatusChip } from '@rbgrid/design-system';
import { QuickTariff } from '../components/onboarding/QuickTariff';
import { SiteNamePanel } from '../components/settings/SiteNamePanel';
import { TimePanel, tzName } from '../components/settings/TimePanel';
import { useOnboarding, StepKey, OnboardingContext } from '../onboarding/useOnboarding';
import { useConnection } from '../hooks/useConnection';
import { useSensors } from '../hooks/useSensors';
import { getApiClient } from '../services/api';
import { getBaseUrl } from '../services/connection';
import { useDsTokens } from '../theme/useDsTokens';
import { SecurityPanel } from '../components/settings/SecurityPanel';
import { ShadowConfigEditor } from '../components/settings/ShadowConfigEditor';
import { NodesPanel } from '../components/devices/NodesPanel';
import { BindUnitDialog } from '../components/devices/BindUnitDialog';
import { SensorListEntry, freeSensors, channelMissing } from '../types/sensors';
import { t } from '../i18n';
import { useUnitsConfig } from '../hooks/useUnitsConfig';
import { fullDisplayName } from '../entities/siteModel';
import type { UnitEntityGroup } from '../entities/model';
import { useConfirm } from '../components/common/ConfirmDialog';

/** Step title phrase KEYS (titles stay English in useOnboarding — resolved with t() at render). */
const STEP_TITLE_KEY: Record<StepKey, string> = {
  wifi: 'onboarding.stepWifi',
  security: 'onboarding.stepSecurity',
  object_time: 'onboarding.stepObjectTime',
  sensors: 'onboarding.stepSensors',
  nodes: 'onboarding.stepNodes',
  tariff: 'onboarding.stepTariff',
  integrations: 'onboarding.stepIntegrations',
  done: 'onboarding.stepDone',
};

interface StepProps {
  timeSynced: boolean;
  onChanged: () => void;
  ctx: OnboardingContext;
  goTo: (k: StepKey) => void;
}

/** Step 0 — connection: in device-direct mode we're already on the hub; show it + how to reach the SPA. */
const WifiStep: React.FC = () => {
  const { demo } = useConnection();
  const base = getBaseUrl();
  return (
    <Card>
      <Stack spacing={1.5}>
        <Typography variant="h6" fontWeight={700}>{t('onboarding.wifiTitle')}</Typography>
        <Typography color="text.secondary">
          {demo ? t('onboarding.wifiDemo') : t('onboarding.wifiAddress', { address: base || t('onboarding.wifiThisDevice') })}
        </Typography>
        <Typography variant="body2" color="text.secondary">
          {t('onboarding.wifiHint')}
        </Typography>
      </Stack>
    </Card>
  );
};

/** Step 2 — site & time: system + time config (shadow) + a "sync now" trigger + tq readout. */
const ObjectTimeStep: React.FC<{ timeSynced: boolean; onChanged: () => void }> = ({ onChanged }) => {
  return (
    <Stack spacing={2}>
      {/* What kind of site this is — shapes the Overview (review 11 §7.1: scenario cards with art). */}
      <SiteProfilePanel />
      {/* Two fields and the zone, each with its own «Save» — the generic system/time editors
          (~35 service fields, no save button, time read-only on the wire) saved nothing (W191-01/02). */}
      <SiteNamePanel onSaved={onChanged} />
      <TimePanel compact />
    </Stack>
  );
};

/** Step 3 — sensors: list sensors (live state + role), create a point from any unbound one. */
const SensorsStep: React.FC<{ onChanged: () => void }> = ({ onChanged }) => {
  const { sensors, modules, state, reload } = useSensors();
  const unitsCfg = useUnitsConfig();
  const pointName = (k: string) => { const g = { unitKey: k } as UnitEntityGroup; return fullDisplayName(g, unitsCfg.forGroup(g)); };
  const [binding, setBinding] = useState<SensorListEntry | null>(null);
  const bound = sensors.filter((s) => s.unit_id != null);
  const unbound = freeSensors(sensors, modules);

  return (
    <Card>
      <Stack spacing={1.5}>
        <Typography variant="h6" fontWeight={700}>{t('onboarding.sensorsTitle')}</Typography>
        {state === 'loading' || state === 'initializing' ? (
          <Stack direction="row" spacing={1} alignItems="center"><CircularProgress size={18} aria-label={t('common.loading')} /><Typography color="text.secondary">{t('onboarding.hubStarting')}</Typography></Stack>
        ) : (
          <>
            <Typography variant="body2" color="text.secondary">
              {t('onboarding.sensorsSummary', { bound: bound.length, unbound: unbound.length })}
            </Typography>
            <Stack spacing={1}>
              {sensors.filter((s) => s.unit_id != null || !channelMissing(s, modules)).map((s, i) => (
                <Stack key={i} direction="row" alignItems="center" spacing={1}>
                  {s.unit_id == null ? <StatusChip status="dormant" label={t('devices.chFree')} />
                    : <StatusChip status={s.present ? 'present' : 'dormant'} label={s.present ? t('devices.online') : t('devices.noSignal')} />}
                  {/* the point by its name first, the wiring small (W191-16: «шина 0 · модуль 0 · I0 → точка 0006») */}
                  <Box sx={{ minWidth: 0 }}>
                    <Typography variant="body2" fontWeight={600}>{s.unit_id != null ? pointName(s.unit_id) : t('onboarding.sensorFree')}</Typography>
                    <Typography sx={{ fontSize: 12, color: 'text.secondary' }}>
                      {t('onboarding.sensorI2c', { bus: s.bus, module: s.module, ch: channelLabel(s.ch, s.ch_label) })}
                    </Typography>
                  </Box>
                  {s.unit_id == null && (
                    <Button size="small" variant="outlined" sx={{ ml: 'auto' }} onClick={() => setBinding(s)}>
                      {t('onboarding.createPoint')}
                    </Button>
                  )}
                </Stack>
              ))}
              {sensors.length === 0 && <Typography color="text.secondary">{t('onboarding.noSensors')}</Typography>}
            </Stack>
          </>
        )}
      </Stack>
      <BindUnitDialog sensor={binding} onClose={() => setBinding(null)} onBound={() => { setBinding(null); reload(); onChanged(); }} />
    </Card>
  );
};

/** A single tariff template suggestion card (opens the Tariffs screen to apply). */
const TariffTemplate: React.FC<{ title: string; desc: string; onOpen: () => void }> = ({ title, desc, onOpen }) => (
  <Card outlined>
    <Stack spacing={0.75}>
      <Typography sx={{ fontWeight: 700, fontSize: 14 }}>{title}</Typography>
      <Typography variant="body2" color="text.secondary">{desc}</Typography>
      <Button size="small" variant="text" sx={{ alignSelf: 'flex-start', px: 0 }} onClick={onOpen}>
        {t('onboarding.configure')}
      </Button>
    </Stack>
  </Card>
);

/** Step 5 — tariff: offer common plan templates; the actual editor lives on the Tariffs screen. */
const TariffStep: React.FC<{ onChanged?: () => void }> = ({ onChanged }) => {
  const navigate = useNavigate();
  const open = () => navigate('/tariffs');
  return (
    <Card>
      <Stack spacing={1.5}>
        <Typography variant="h6" fontWeight={700}>{t('onboarding.tariffTitle')}</Typography>
        <Typography color="text.secondary">
          {t('onboarding.tariffBody')}
        </Typography>
        {/* Entered right here, as the bill prints it (W191-07/08: templates only opened the full
            editor on the factory test plan). The full editor stays one link away. */}
        <QuickTariff onSaved={onChanged} />
        <Button variant="text" sx={{ alignSelf: 'flex-start' }} onClick={open}>{t('onboarding.openTariffs')}</Button>
      </Stack>
    </Card>
  );
};

/** Integrations — what they are and where to set them; the raw MQTT fields stay in Settings
 *  (W191-14: «Effective uri», a broker nobody set, «saved: off / connected: on» in English). */
const IntegrationsStep: React.FC = () => {
  const navigate = useNavigate();
  return (
    <Card>
      <Stack spacing={1.5}>
        <Typography variant="h6" fontWeight={700}>{t('onboarding.integrationsTitle')}</Typography>
        <Typography color="text.secondary">{t('onboarding.integrationsBody')}</Typography>
        <Button variant="outlined" sx={{ alignSelf: 'flex-start' }} onClick={() => navigate('/config#mqtt')}>{t('onboarding.integrationsOpen')}</Button>
      </Stack>
    </Card>
  );
};

/** Step 7 — done: summary of the derived setup state + optional reboot-with-progress + finish. */
const DoneStep: React.FC<{ ctx: OnboardingContext; goTo: (k: StepKey) => void }> = ({ ctx, goTo }) => {
  const navigate = useNavigate();
  // What the user set, each with its state and a way back (W191-10: «configured» while nothing was
  // written; «Points: 0» flashed while loading; a reboot button nobody needed).
  const wait = !ctx.loaded;
  const rows: { label: string; ok: boolean; text: string; step: StepKey; required: boolean }[] = [
    { label: t('onboarding.sumSite'), ok: ctx.siteNamed, text: ctx.siteName ?? t('onboarding.notSet'), step: 'object_time', required: true },
    { label: t('onboarding.sumZone'), ok: !!ctx.timezone && ctx.timezone !== 'UTC0', text: ctx.timezone ? tzName(ctx.timezone) : '…', step: 'object_time', required: false },
    { label: t('onboarding.sumTime'), ok: ctx.timeSynced, text: ctx.timeSynced ? t('onboarding.synced') : t('onboarding.notSynced'), step: 'object_time', required: true },
    ...(ctx.isFull ? [{ label: t('onboarding.sumTariff'), ok: ctx.tariffSet, text: ctx.tariffSet ? t('onboarding.tariffOwn') : t('onboarding.tariffExample'), step: 'tariff' as StepKey, required: true }] : []),
    { label: t('onboarding.sumPoints'), ok: ctx.pointCount > 0, text: wait ? '…' : `${ctx.pointCount}`, step: 'sensors', required: true },
    { label: t('onboarding.sumSecurity'), ok: ctx.adminTokenSet, text: ctx.adminTokenSet ? t('onboarding.tokenSet') : t('onboarding.tokenNotSet'), step: 'security', required: false },
  ];
  const left = wait ? [] : rows.filter((r) => r.required && !r.ok);

  return (
    <Card>
      <Stack spacing={2} alignItems="flex-start">
        <Typography variant="h6" fontWeight={700}>{left.length ? t('onboarding.almostTitle') : t('onboarding.doneTitle')}</Typography>
        <Typography color="text.secondary">
          {left.length ? t('onboarding.almostBody', { list: left.map((r) => r.label).join(', ') }) : t('onboarding.doneBody')}
        </Typography>

        <Stack spacing={0.75} sx={{ width: '100%' }}>
          {rows.map((r) => (
            <Stack key={r.label} direction="row" alignItems="center" spacing={1.5} flexWrap="wrap" useFlexGap>
              <Typography variant="body2" sx={{ minWidth: 140 }}>{r.label}</Typography>
              <StatusChip status={wait ? 'dormant' : r.ok ? 'live' : 'dormant'} label={r.text} />
              {!wait && !r.ok && <Button size="small" onClick={() => goTo(r.step)}>{t('onboarding.fix')}</Button>}
            </Stack>
          ))}
        </Stack>

        <Button variant="contained" onClick={() => navigate('/dashboard')}>{t('onboarding.goOverview')}</Button>
      </Stack>
    </Card>
  );
};

const STEP_CONTENT: Record<StepKey, (p: StepProps) => React.ReactNode> = {
  wifi: () => <WifiStep />,
  security: () => (
    <Stack spacing={1.5}>
      <Alert severity="info">{t('onboarding.securityWhy')}</Alert>
      <SecurityPanel />
    </Stack>
  ),
  object_time: ({ timeSynced, onChanged }) => <ObjectTimeStep timeSynced={timeSynced} onChanged={onChanged} />,
  sensors: ({ onChanged }) => <SensorsStep onChanged={onChanged} />,
  nodes: () => <NodesPanel />,
  tariff: ({ onChanged }) => <TariffStep onChanged={onChanged} />,
  integrations: () => <IntegrationsStep />,
  done: ({ ctx, goTo }) => <DoneStep ctx={ctx} goTo={goTo} />,
};

export const OnboardingPage: React.FC = () => {
  const ds = useDsTokens();
  const ob = useOnboarding();
  const theme = useTheme();
  // < 820: an 8-step horizontal stepper doesn't fit → «Шаг N из 8 · …» + progress (review 03 P2-20).
  const narrow = useMediaQuery(theme.breakpoints.down('md'));
  if (!ob.current) return <Box sx={{ display: 'flex', justifyContent: 'center', py: 10 }}><CircularProgress aria-label={t('common.loading')} /></Box>;

  const { steps, index, current, isComplete, ctx } = ob;
  const required = !current.optional && !isComplete(current) && current.key !== 'done';

  return (
    <Box>
      <Stack direction="row" alignItems="center" spacing={1.5} sx={{ mb: 2.5 }}>
        <Typography variant="h4" fontWeight={800}>{t('onboarding.title')}</Typography>
        {!ob.loaded && <CircularProgress size={18} aria-label={t('common.loading')} />}
      </Stack>

      {/* The step set settles once the hub answered (the ESP-NOW step came and went: 8 ↔ 7 steps,
          clicks missed — W191-09): hold the stepper's place until then. */}
      {!ob.loaded ? <Box sx={{ mb: 3, minHeight: 40 }} /> : narrow ? (
        <Box sx={{ mb: 3 }}>
          <Stack direction="row" justifyContent="space-between" alignItems="baseline" sx={{ mb: 0.75 }}>
            <Typography sx={{ fontWeight: 600, color: ds.ink }}>
              {t('onboarding.stepOf', { n: index + 1, total: steps.length, title: t(STEP_TITLE_KEY[current.key]) })}
            </Typography>
            {current.optional && <Typography variant="caption" sx={{ color: ds.inkMuted }}>{t('onboarding.skippable')}</Typography>}
          </Stack>
          <LinearProgress
            variant="determinate"
            value={((index + 1) / steps.length) * 100}
            aria-label={t('onboarding.progress')}
            sx={{ height: 6, borderRadius: 1.5, bgcolor: ds.surfaceRaised, '& .MuiLinearProgress-bar': { bgcolor: ds.primary } }}
          />
        </Box>
      ) : (
      <Stepper
        nonLinear
        activeStep={index}
        sx={{
          mb: 3,
          flexWrap: 'wrap',
          gap: 1,
          '& .MuiStepIcon-root': { color: ds.borderStrong },
          '& .MuiStepIcon-root.Mui-active': { color: ds.primary },
          '& .MuiStepIcon-root.Mui-completed': { color: ds.statusOnline },
          '& .MuiStepLabel-label': { color: ds.inkMuted },
          '& .MuiStepLabel-label.Mui-active': { color: ds.ink, fontWeight: 700 },
          '& .MuiStepLabel-label.Mui-completed': { color: ds.ink },
        }}
      >
        {steps.map((s, i) => (
          <Step key={s.key} completed={isComplete(s)}>
            <StepButton
              onClick={() => ob.goto(i)}
              optional={s.optional ? (
                <Typography variant="caption" sx={{ color: ds.inkMuted }}>{t('onboarding.skippable')}</Typography>
              ) : undefined}
            >
              {t(STEP_TITLE_KEY[s.key])}
            </StepButton>
          </Step>
        ))}
      </Stepper>
      )}

      <Box sx={{ mb: 2 }}>
        {STEP_CONTENT[current.key]({ timeSynced: ctx.timeSynced, onChanged: ob.refresh, ctx, goTo: (k) => ob.goto(Math.max(0, steps.findIndex((x) => x.key === k))) })}
      </Box>

      {required && (
        <Alert severity="info" sx={{ mb: 2 }}>
          {t('onboarding.stepIncomplete')}
        </Alert>
      )}

      <Divider sx={{ my: 2 }} />
      <Stack direction="row" spacing={1}>
        <Button variant="outlined" disabled={index === 0} onClick={ob.back}>{t('common.back')}</Button>
        <Box sx={{ flex: 1 }} />
        {current.optional && index < steps.length - 1 && (
          <Button onClick={ob.next}>{t('onboarding.skip')}</Button>
        )}
        {index < steps.length - 1 && (
          <Button variant="contained" onClick={ob.next}>
            {isComplete(current) ? t('common.next') : t('onboarding.continue')}
          </Button>
        )}
        {index === steps.length - 1 && <Chip color="success" label={t('onboarding.finished')} />}
      </Stack>
    </Box>
  );
};
