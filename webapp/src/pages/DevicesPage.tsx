/**
 * DevicesPage — «Точки»: реестр точек учёта. Привязанные точки показываются живыми
 * PointTile (джойн с телеметрией) и ведут на детальную страницу /point/<id>, у каждой —
 * русифицированная отвязка; свободные датчики предлагают привязку. Гейт готовности
 * (503 «инициализация» → повтор) отличаем от «датчиков нет».
 */
import { useTariffPlan } from '../hooks/useTariffPlan';
import { channelLabel } from '../entities/phaseGroup';
import React, { useMemo, useState } from 'react';
import {
  Box, Typography, Grid, Button, Stack, Alert, CircularProgress,
} from '@mui/material';
import { useNavigate } from 'react-router-dom';
import { Card, StatusChip } from '@rbgrid/design-system';
import AddLinkIcon from '@mui/icons-material/AddLink';
import LinkOffIcon from '@mui/icons-material/LinkOff';
import RefreshIcon from '@mui/icons-material/Refresh';
import { useSensors } from '../hooks/useSensors';
import { isBasic, isCurrentOnly, ctName, ctRatingOf, channelRange, sensorClassOf } from '../entities/moduleLine';
import { nf } from '../components/common/ruFormat';
import { useTelemetryEntities } from '../hooks/useTelemetryEntities';
import { useUnitsConfig } from '../hooks/useUnitsConfig';
import { displayName, fullDisplayName } from '../entities/siteModel';
import { useCapabilities } from '../core';
import { getApiClient } from '../services/api';
import { SensorListEntry, SensorModule, freeSensors, channelMissing, sctUncorrected } from '../types/sensors';
import { UnitEntityGroup, unitMetricValue } from '../entities/model';
import { BindUnitDialog } from '../components/devices/BindUnitDialog';
import { ModuleFirmwareDialog } from '../components/devices/ModuleFirmwareDialog';
import { AddModuleDialog } from '../components/devices/AddModuleDialog';
import { isDemo } from '../services/connection';
import { refreshHardware } from '../hooks/useHardware';
import { RELEASE_HIDDEN } from '../constants/release';
import AddIcon from '@mui/icons-material/Add';
import { fwLabel } from '../components/devices/moduleFw';
import { isPhaseGroup, inputGroupOffer, PHASE_GROUP_CH, VOLTAGE_INPUT_CH } from '../entities/phaseGroup';
import { fwAtLeast } from '../entities/hubVersion';
import { useHubVersion } from '../hooks/useHubVersion';
import { RebootNowButton } from '../components/common/RebootNowButton';
import { NodesPanel } from '../components/devices/NodesPanel';
import { PointTile } from '../components/dashboard/PointTile';
import { useDsTokens } from '../theme/useDsTokens';
import { t, tCode } from '../i18n';
import { MoneyLensProvider } from '../economics/MoneyLens';
import { liveRate, dayAvgRate, solarDayOf } from '../economics/costRate';
import { useMoneyFormat } from '../hooks/useMoneyFormat';
import { useConfirm } from '../components/common/ConfirmDialog';
import { isSupplyRole, signedPowerW, isPseudoKey, isMainKey } from '../entities/siteModel';
import { mainUnit, isGenerationUnit } from './overviewData';

const tnum = { fontVariantNumeric: 'tabular-nums' } as const;

function addressing(s: SensorListEntry): string {
  if (isPhaseGroup(s)) {
    return t('devices.addrI2cGroup', { bus: s.bus, module: s.module, label: (s as { ch_label?: string }).ch_label || 'L1–L3' }) + (s.addr !== undefined ? ` · 0x${s.addr.toString(16)}` : '');
  }
  return t('devices.addrI2c', { bus: s.bus, module: s.module, ch: channelLabel(s.ch, s.ch_label) }) + (s.addr !== undefined ? ` · 0x${s.addr.toString(16)}` : '');
}

/** «3U + 3I» from the module's channel counts; '' when unknown. */
function channelsText(m: SensorModule): string {
  const parts = [m.n_v ? `${m.n_v}U` : '', m.n_i ? `${m.n_i}I` : ''].filter(Boolean);
  return parts.join(' + ');
}

/** «занято 3 из 7» — current channels bound to a point (a group counts all); '' when unknown. */
export function occupancyText(m: SensorModule, sensors: SensorListEntry[]): string {
  if (!m.present || !m.n_i || m.n_i < 2) return '';
  const own = sensors.filter((s) => s.module === m.module && s.ch != null && s.ch !== VOLTAGE_INPUT_CH);
  const used = own.some((s) => s.ch === PHASE_GROUP_CH && s.unit_id) ? m.n_i
    : own.filter((s) => s.unit_id && (s.ch as number) < (m.n_i as number)).length;
  return t('devices.chOccupied', { n: used, m: m.n_i });
}

/** Calibration marker → words. "not_asserted" today is a known false negative of the module library
 *  (rbamp-dev via root 16.10: «do not gate») — said neutrally, no warning. */
const calText = (cal?: string): string =>
  cal === 'ok' ? t('devices.calOk') : cal === 'not_asserted' ? t('devices.calNotAsserted') : t('devices.calUnknown');

const Centered: React.FC<{ children: React.ReactNode }> = ({ children }) => (
  <Box sx={{ display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', py: 10, gap: 2 }}>
    {children}
  </Box>
);

export const DevicesPage: React.FC = () => {
  const { sensors, modules, state, busStalledS, busLineLowS, reload } = useSensors();
  const [fwModule, setFwModule] = useState<SensorModule | null>(null);
  const [addingModule, setAddingModule] = useState(false);
  // v1.1.2 (root's .139 run 03.11): hubs ≥ 1.1.2 — older ones stop at 3 modules and leave a just-added
  // module's channels «no signal».
  const hubVersionEarly = useHubVersion();
  const canAddModule = !isDemo() && !RELEASE_HIDDEN.addModule && fwAtLeast(hubVersionEarly, [1, 1, 2]) === true;
  const telem = useTelemetryEntities();
  const { plan: tariffPlan } = useTariffPlan();
  const unitsCfg = useUnitsConfig();
  const mf = useMoneyFormat();
  const cfgOf = (key: string) => unitsCfg.forGroup({ unitKey: key } as UnitEntityGroup);
  const caps = useCapabilities();
  const navigate = useNavigate();
  const [bindTarget, setBindTarget] = useState<SensorListEntry | null>(null);
  const [busyId, setBusyId] = useState<string | null>(null);
  const [unbindErr, setUnbindErr] = useState<string | null>(null);
  const confirm = useConfirm();
  const ds = useDsTokens();

  // The input on all three phases of a U3I3 (v1.1.2 rebind; hidden until root's .139 run).
  const hubVersion = hubVersionEarly;
  const groupOffer = useMemo(
    () => (RELEASE_HIDDEN.inputGroup ? new Map<number, 'regroup' | 'update'>() : inputGroupOffer(sensors, modules, unitsCfg.input.hex ?? null, fwAtLeast(hubVersion, [1, 1, 2]))),
    [sensors, modules, unitsCfg.input.hex, hubVersion],
  );
  const [groupMsg, setGroupMsg] = useState<{ sev: 'warning' | 'error'; text: string; reboot?: boolean } | null>(null);
  const regroupInput = async (m: SensorModule) => {
    const hex = unitsCfg.input.hex;
    if (!hex) return;
    if (!(await confirm({ title: t('devices.inputGroupTitle', { model: m.model ?? '' }), body: t('devices.inputGroupBody'), confirmLabel: t('devices.inputGroupConfirm') }))) return;
    setBusyId(`group-${m.module}`); setGroupMsg(null);
    try {
      const r = await getApiClient().rebindInputGroup(hex, m.module);
      setGroupMsg(r.status === 'ok'
        ? { sev: 'warning', text: t('devices.inputGroupDone'), reboot: true }
        : { sev: 'error', text: t('devices.inputGroupRefused', { why: r.message ?? r.code ?? '' }) });
    } catch (e) {
      setGroupMsg({ sev: 'error', text: t('devices.inputGroupRefused', { why: e instanceof Error ? e.message : String(e) }) });
    } finally { setBusyId(null); }
  };
  const bound = sensors.filter((s) => s.unit_id !== null);
  const unbound = freeSensors(sensors, modules);
  const modelOf = (s: SensorListEntry): string | undefined => modules.find((m) => m.module === s.module)?.model;

  // Join the live entity model so a bound point can render as a live PointTile.
  const groupByKey = useMemo(() => {
    const m = new Map<string, UnitEntityGroup>();
    telem.data?.units.forEach((g) => m.set(g.unitKey, g));
    return m;
  }, [telem.data]);
  const sensorKeys = useMemo(() => new Set(sensors.map((x) => (x.unit_id ?? '').toUpperCase()).filter(Boolean)), [sensors]);
  const otherPoints = useMemo(
    () => (telem.data?.units ?? []).filter((g) => !isPseudoKey(g.unitKey) && !isMainKey(g.unitKey) && !sensorKeys.has(g.unitKey.toUpperCase())),
    [telem.data, sensorKeys],
  );

  const unbind = async (s: SensorListEntry) => {
    if (!s.unit_id) return;
    // the factory «Main Voltage Bus» in the UI language (W191-16), as everywhere else
    const name = fullDisplayName({ unitKey: s.unit_id } as UnitEntityGroup, cfgOf(s.unit_id));
    if (!(await confirm({ title: t('devices.unbindTitle', { name }), body: t('devices.unbindBody'), confirmLabel: t('devices.unbind'), danger: true }))) return;
    setBusyId(s.unit_id);
    setUnbindErr(null);
    try {
      const res = await getApiClient().deleteUnit(s.unit_id);
      if (res.status !== 'ok') setUnbindErr(res.message ?? t('devices.unbindFailed'));
    } catch {
      setUnbindErr(t('devices.unbindRequestFailed'));
    } finally {
      setBusyId(null);
      reload();
    }
  };

  if (state === 'loading') {
    return <Centered><CircularProgress aria-label={t('common.loading')} /></Centered>;
  }
  if (state === 'initializing') {
    return (
      <Centered>
        <CircularProgress aria-label={t('common.loading')} />
        <Typography color="text.secondary">{t('devices.initializing')}</Typography>
      </Centered>
    );
  }
  if (state === 'error') {
    // keep the error state; show a Russian message, not the hook's raw English string
    return (
      <Alert severity="error" action={<Button color="inherit" size="small" onClick={reload}>{t('common.retry')}</Button>}>
        {t('devices.loadFailed')}
      </Alert>
    );
  }

  // The main input and the voltage bus are the site's frame of reference — never one tap from
  // the points list (UX audit PNT-17); they are re-bound through the setup wizard.
  const unbindButton = (s: SensorListEntry) => {
    const cfg = cfgOf(s.unit_id ?? '');
    if (isSupplyRole(cfg?.role) || cfg?.type === 'VOLTAGE_BUS') return null;
    return (
    <Button
      size="small" color="error" fullWidth startIcon={<LinkOffIcon />}
      disabled={busyId === s.unit_id} onClick={() => unbind(s)}
    >
      {busyId === s.unit_id ? t('devices.unbinding') : t('devices.unbind')}
    </Button>
    );
  };

  // Tiles here get the same money line as on the Overview — currency and the live rate
  // (UX audit PNT-19/GEN-13: «≈ 0/ч» with no currency outside the dashboard's lens).
  const lensRate = liveRate(telem.data?.system.totals?.tariff, tariffPlan);
  const lensMain = telem.data ? mainUnit(telem.data.units, unitsCfg) : undefined;
  const lensGridW = lensMain?.present ? signedPowerW(lensMain) : null;
  return (
    <MoneyLensProvider mode="power" rate={lensRate} fmt={mf.money} gridW={lensGridW} dayRate={dayAvgRate(telem.data?.system.totals)}
      solarDay={solarDayOf((telem.data?.units ?? []).filter((g) => isGenerationUnit(g, unitsCfg)).reduce((s, g) => s + (unitMetricValue(g, 'energy_day_out') ?? 0), 0),
        telem.data?.system.totals, dayAvgRate(telem.data?.system.totals).buy ?? lensRate.buy, lensRate.sell)}>
    <Box>
      <Box sx={{ display: 'flex', alignItems: 'center', flexWrap: 'wrap', rowGap: 1, mb: 2.5 }}>
        <Typography variant="h4" fontWeight={800}>{t('devices.title')}</Typography>
        {/* bus 0, up to the hub's module limit (v1.1.2, operator 02.11); the demo has no bus */}
        {canAddModule && <Button startIcon={<AddIcon />} onClick={() => setAddingModule(true)} sx={{ ml: 'auto' }}>{t('devices.addModule')}</Button>}
        <Button startIcon={<RefreshIcon />} onClick={reload} sx={{ ml: canAddModule ? 1 : 'auto' }}>{t('devices.refresh')}</Button>
      </Box>

      {/* v1.1.2: the hub polled no module for N s — a module wired without power clamps the bus, and it
          stays stuck after unplugging until a restart (root 03.11, rbamp-dev bench). */}
      {/* A held-low line clears by itself when released — no restart (root 03.11, RC 0c7a397c). */}
      {busLineLowS != null && busStalledS == null && <Alert severity="error" sx={{ mb: 2 }}>{t('devices.busLineLow')}</Alert>}
      {busStalledS != null && <Alert severity="error" action={<RebootNowButton />} sx={{ mb: 2 }}>{t('devices.busStalled', { s: busStalledS })}</Alert>}
      {groupMsg && <Alert severity={groupMsg.sev} onClose={() => setGroupMsg(null)} action={groupMsg.reboot ? <RebootNowButton /> : undefined} sx={{ mb: 2 }}>{groupMsg.text}</Alert>}
      {unbindErr && <Alert severity="error" onClose={() => setUnbindErr(null)} sx={{ mb: 2 }}>{unbindErr}</Alert>}

      {caps.flags.espnow && <NodesPanel />}

      {/* rbAmp modules as identified by the hub (delta №5) — model, address, channels, calibration. */}
      {modules.length > 0 && (
        <>
          <Typography variant="overline" color="text.secondary" sx={tnum}>{t('devices.modulesHeader', { n: modules.length })}</Typography>
          <Grid container spacing={2} sx={{ mb: 3, mt: 0 }}>
            {modules.map((m) => (
              <Grid item xs={12} sm={6} md={4} key={m.module}>
                <Card>
                  <Stack spacing={0.5}>
                    <Stack direction="row" alignItems="center" justifyContent="space-between" spacing={1}>
                      <Typography sx={{ fontWeight: 700, fontSize: 15 }}>{m.model || t('devices.moduleUnknown')}</Typography>
                      <StatusChip status={m.present ? 'present' : 'dormant'} label={m.present ? t('devices.online') : t('devices.moduleAbsent')} />
                    </Stack>
                    <Typography sx={{ ...tnum, fontSize: 13, color: ds.inkMuted }}>
                      {[t('devices.moduleN', { module: m.module }), m.addr !== undefined ? `0x${m.addr.toString(16)}` : '', channelsText(m), occupancyText(m, sensors)].filter(Boolean).join(' · ')}
                    </Typography>
                    {/* BASIC has no calibration register (cal "n/a", d03f19ed): its sensor_class says it. */}
                    {m.cal !== 'n/a' && <Typography sx={{ fontSize: 13, color: m.cal === 'ok' ? ds.ink : ds.inkMuted }}>{calText(m.cal)}</Typography>}
                    {fwLabel(m) && <Typography sx={{ ...tnum, fontSize: 12, color: ds.inkMuted }}>{t('modfw.version', { fw: fwLabel(m) })}</Typography>}
                    {/* BASIC (root dd861a54): say what it can't do before anyone wonders. */}
                    {isBasic(m) && (
                      <Typography sx={{ fontSize: 12, color: ds.inkMuted, mt: 0.5 }}>
                        {t(isCurrentOnly(m) ? 'devices.currentOnlyNote' : 'devices.basicNote')}
                      </Typography>
                    )}
                    {/* The sensor on each current channel (SCT-: chosen per channel; CT-: factory-paired). */}
                    {sensorClassOf(m) === 'uncalibrated' && (
                      <Typography sx={{ fontSize: 12, color: ds.sevWarning }}>{t('devices.uncalibrated')}</Typography>
                    )}
                    {sensorClassOf(m) !== 'uncalibrated' && sensors.filter((s) => s.module === m.module && (s.ct_code ?? 0) > 0 && !channelMissing(s, modules)).map((s) => {
                      const rating = ctRatingOf(s, { ...caps.ctModels, ...caps.ctModelsFixed });
                      const r = rating ? channelRange(s, rating, sensorClassOf(m)) : null;
                      return (
                        <Typography key={`${s.module}-${s.ch}`} sx={{ ...tnum, fontSize: 12, color: ds.inkMuted }}>
                          {t('devices.channelSensor', { ch: channelLabel(s.ch, s.ch_label), sensor: s.ct_name || ctName(s.ct_code, m, caps.ctModels, caps.ctModelsFixed) })}
                          {r ? ` · ${t('devices.ctRange', { lo: nf(r.lo, 2), hi: nf(r.hi, 0) })}` : ''}
                          {/* a module may be wired partly, any channels (operator 18.10): free is normal, not an alarm */}
                          {s.unit_id ? ` → ${fullDisplayName({ unitKey: s.unit_id } as UnitEntityGroup, cfgOf(s.unit_id))}` : ` · ${t('devices.chFree')}`}
                          {s.sct_correction === 'corrected' && typeof s.sct_scale === 'number' ? ` · ${t('devices.sctScale', { k: nf(s.sct_scale, 3) })}` : ''}
                          {/* rbamp 1.7.2: readings not corrected for the CT — said at the channel (root 20.10) */}
                          {sctUncorrected(s) && (
                            <Box component="span" sx={{ display: 'block', color: ds.sevWarning, fontWeight: 600 }}>
                              ⚠ {t('devices.sctUncorrected', { why: tCode('sct', String(s.sct_correction)) })}
                            </Box>
                          )}
                        </Typography>
                      );
                    })}
                    {/* STD modules the hub can update over Wi-Fi (root 534ded9e) */}
                    {m.ota && m.present && (
                      <Button size="small" variant="outlined" sx={{ alignSelf: 'flex-start', mt: 1 }} onClick={() => setFwModule(m)}>{t('modfw.update')}</Button>
                    )}

                    {/* Binding a NEW point as a group (POST /units ch 254) is gone: every hub refuses it (root 03.11). */}
                    {groupOffer.get(m.module) === 'regroup' && (
                      <Button
                        size="small" variant="contained" startIcon={<AddLinkIcon />} sx={{ alignSelf: 'flex-start', mt: 1 }}
                        disabled={busyId !== null} onClick={() => regroupInput(m)}
                      >
                        {t('devices.inputGroup')}
                      </Button>
                    )}
                    {groupOffer.get(m.module) === 'update' && (
                      <Typography sx={{ fontSize: 12, color: ds.inkMuted, mt: 0.5 }}>{t('devices.inputGroupUpdate', { model: m.model ?? '' })}</Typography>
                    )}
                  </Stack>
                </Card>
              </Grid>
            ))}
          </Grid>
        </>
      )}

      {sensors.length === 0 && (
        <Card outlined>
          <Box sx={{ py: 3, textAlign: 'center', color: 'text.secondary' }}>
            <Typography>{t('devices.noSensors')}</Typography>
          </Box>
        </Card>
      )}

      {bound.length > 0 && (
        <>
          <Typography variant="overline" color="text.secondary" sx={tnum}>{t('devices.boundHeader', { n: bound.length })}</Typography>
          <Grid container spacing={2} sx={{ mb: 3, mt: 0 }}>
            {bound.map((s) => {
              const key = s.unit_id ?? '';
              const group = groupByKey.get(key);
              const go = () => navigate(`/point/${key}`);
              return (
                <Grid item xs={12} sm={6} md={4} key={key}>
                  <Stack spacing={1}>
                    {group ? (
                      <PointTile group={group} name={displayName(group, unitsCfg.forGroup(group))} onClick={go} generation={isGenerationUnit(group, unitsCfg)} cfg={unitsCfg.forGroup(group)} />
                    ) : (
                      <Card muted={!s.present}>
                        <Box
                          onClick={go} role="button" tabIndex={0}
                          sx={{ cursor: 'pointer' }}
                        >
                          <Stack direction="row" justifyContent="space-between" alignItems="center" mb={1}>
                            <Typography variant="subtitle1" fontWeight={700} sx={{ color: ds.ink }}>
                              {fullDisplayName({ unitKey: key } as UnitEntityGroup, cfgOf(key))}
                            </Typography>
                            <StatusChip
                              status={s.present ? 'present' : 'dormant'}
                              label={s.present ? t('devices.online') : t('devices.noSignal')}
                            />
                          </Stack>
                          <Typography variant="caption" color="text.secondary" display="block" sx={tnum}>
                            {addressing(s)}
                          </Typography>
                        </Box>
                      </Card>
                    )}
                    {/* Which hardware measures this point (UX audit PNT-14: «какой канал — какая линия»
                        was only in the technical details). */}
                    {group && (
                      <Typography variant="caption" color="text.secondary" sx={{ ...tnum, px: 0.5 }}>
                        {addressing(s)}{modelOf(s) ? ` · ${modelOf(s)}` : ''}
                      </Typography>
                    )}
                    {unbindButton(s)}
                  </Stack>
                </Grid>
              );
            })}
          </Grid>
        </>
      )}

      {/* Points that aren't on a local sensor channel — ESP-NOW stations etc. — are points too
          (PNT-04: they were on the Overview but nowhere to open or rename). */}
      {otherPoints.length > 0 && (
        <>
          <Typography variant="overline" color="text.secondary" sx={tnum}>{t('devices.otherHeader', { n: otherPoints.length })}</Typography>
          <Grid container spacing={2} sx={{ mb: 3, mt: 0 }}>
            {otherPoints.map((g) => (
              <Grid item xs={12} sm={6} md={4} key={g.unitKey}>
                <PointTile group={g} name={displayName(g, unitsCfg.forGroup(g))} onClick={() => navigate(`/point/${g.unitKey}`)}
                  generation={isGenerationUnit(g, unitsCfg)} cfg={unitsCfg.forGroup(g)} />
              </Grid>
            ))}
          </Grid>
        </>
      )}

      {/* No free channel (PNT-05): say so and how to get one instead of an empty section. */}
      {sensors.length > 0 && unbound.length === 0 && (
        <Typography variant="body2" color="text.secondary" sx={{ mb: 2 }}>{t('devices.noFreeChannels')}</Typography>
      )}

      {unbound.length > 0 && (
        <>
          <Typography variant="overline" color="text.secondary" sx={tnum}>{t('devices.unboundHeader', { n: unbound.length })}</Typography>
          <Grid container spacing={2} sx={{ mt: 0 }}>
            {unbound.map((s, i) => (
              <Grid item xs={12} sm={6} md={4} key={`${addressing(s)}-${i}`}>
                <Card outlined>
                  <Stack direction="row" justifyContent="space-between" alignItems="center" mb={1}>
                    <Typography variant="subtitle1" fontWeight={700} sx={{ color: ds.ink }}>{t('devices.freeSensor')}</Typography>
                    <StatusChip status="dormant" label={t('devices.chFree')} />
                  </Stack>
                  <Typography variant="caption" color="text.secondary" display="block" sx={tnum}>{addressing(s)}</Typography>
                  <Button size="small" variant="contained" startIcon={<AddLinkIcon />} sx={{ mt: 1.5 }} onClick={() => setBindTarget(s)}>
                    {t('devices.bind')}
                  </Button>
                </Card>
              </Grid>
            ))}
          </Grid>
        </>
      )}

      <AddModuleDialog open={addingModule} onClose={() => { setAddingModule(false); reload(); }} onChanged={() => { reload(); void refreshHardware(); }} />
      <ModuleFirmwareDialog module={fwModule} onClose={() => { setFwModule(null); reload(); }} onDone={reload} />
      <BindUnitDialog
        sensor={bindTarget}
        onClose={() => setBindTarget(null)}
        onBound={() => { setBindTarget(null); reload(); }}
      />
    </Box>
    </MoneyLensProvider>
  );
};
