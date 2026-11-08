/**
 * UnitParamsPanel — edit one point's configuration on its page: display/short name, role,
 * phase, line nominal (for the main input: the input limit) and breaker current. These feed
 * every scenario (generation/EV detection by role, load bars, input-vs-limit, phase cards).
 *
 * Write path (canon a3b5fc62): POST /api/v1/units/{hex} with ONLY the changed fields — live,
 * persisted, atomic, body ≤ 512 B. Older firmware (404/405) → DeviceSession falls back to the
 * full units-config array (≤ ~8 KB). Validation mirrors the hub: short_name / name byte limits
 * from schema.limits (UTF-8 BYTES: f451b6a5 = 24 / 31; older hubs 10 / 31), phase 0..7 offered by the mains system (root delta №4: 1P {0,1} · SPLIT {0,1,2,5} · 3P {0..7}), rated/max ≥ 0 (0 = not set), role from schema enums.role.
 * Supply roles (MAIN/ALTERNATIVE_SUPPLY) are topology — locked both ways (hub: 409
 * ROLE_SUPPLY_LOCKED); a role change applies live, only the history depth follows after a reboot.
 */
import React, { useEffect, useMemo, useState } from 'react';
import { HelpTip } from '../common/HelpTip';
import { Alert, Box, Button, Checkbox, FormControlLabel, Link, MenuItem, Stack, TextField, Typography } from '@mui/material';
import { useDeviceGroups } from '../../hooks/useDeviceGroups';
import { useUnitsConfig } from '../../hooks/useUnitsConfig';
import { useGridSystem } from '../../hooks/useGridSystem';
import { phaseChoices, GridSystem, phaseCodeValidated, validatedKinds } from '../../entities/gridSystem';
import { Card } from '@rbgrid/design-system';
import { useCapabilities, useConfigShadow, useDeviceSession } from '../../core';
import type { JsonObject } from '../../core';
import { UNIT_ROLE_NAMES } from '../../constants/enums';
import { roleLabel } from '../../pages/overviewData';
import { hexKey } from '../../entities/siteModel';
import { nf } from '../common/ruFormat';
import { utf8Len } from '../common/utf8';
import { t, tCode, intlLocale } from '../../i18n';
import { useUnsavedChanges } from '../../app/unsavedGuard';
import { savedText } from '../../services/savedText';
import { evThresholds } from '../stations/evStatus';
import { useHardware, refreshHardware, markModuleRestart } from '../../hooks/useHardware';
import { pointHardware, ctOptions, ctRatingA, ctRatingOf, channelRange, ctSettable, ctName, sensorClassOf } from '../../entities/moduleLine';
import type { EvConfig } from '../../types/config';

const MAX_FULL_BODY = 8000; // hub body cap for the full units-config POST (~8 KB) — fallback path
export const PHASE_LABEL: Record<number, string> = {
  0: 'unitParams.phaseUnset', 1: 'unitParams.phaseL1', 2: 'unitParams.phaseL2',
  3: 'unitParams.phaseL3', 4: 'unitParams.phaseThree', 5: 'unitParams.phaseL1L2',
  6: 'unitParams.phaseL2L3', 7: 'unitParams.phaseL3L1',
};
/** Phase codes offered: by the DECLARED mains system; a derived one keeps the classic 0..4. */
export const phaseOptions = (grid: GridSystem, current: number): number[] => {
  const base = grid.fromDevice ? phaseChoices(grid.kind) : [0, 1, 2, 3, 4];
  return base.includes(current) ? base : [...base, current];
};
const gridText = (g: GridSystem): string => t(`grid.${g.kind}`, { v: g.nominalV, ll: g.lineV });
const SUPPLY_ROLES = new Set(['MAIN_SUPPLY', 'ALTERNATIVE_SUPPLY']); // topology: not patchable, not assignable

interface Draft { name: string; short_name: string; role: string; phase: number; ratedKw: string; maxA: string; group: number; trunk: boolean; evW: string; evMin: string; ct: number }

/** Roles that export (BASIC can't: consumption only). */
const EXPORT_ROLES = new Set(['RENEWABLE_SOURCE', 'STORAGE_SYSTEM']);

export const UnitParamsPanel: React.FC<{ apiId: string | null }> = ({ apiId }) => {
  const caps = useCapabilities();
  const validated = validatedKinds(caps.stringEnums.grid_system_validated);
  const session = useDeviceSession();
  const deviceGroups = useDeviceGroups();
  const grid = useGridSystem(useUnitsConfig());
  const units = useConfigShadow('units');
  const sysCfg = useConfigShadow('system'); // system.ev: what «0» on a station means
  // The CT model of an rbAmp current channel (root dd861a54): reported in /api/v1/sensors, set by
  // the same per-unit PATCH (ct_code, live, stored in the module).
  const hardware = useHardware();
  const list = useMemo(
    () => (Array.isArray(units.reported.units) ? (units.reported.units as JsonObject[]) : []),
    [units.reported],
  );
  const hwPoint = apiId ? pointHardware(apiId, hardware.sensors, hardware.modules) : { sensor: null, module: null, basic: false };
  const hwSensor = hwPoint.sensor;
  const basicPoint = hwPoint.basic;
  const ctFree = ctSettable(hwPoint.module);
  const sensorCls = sensorClassOf(hwPoint.module);
  const hwCt = typeof hwSensor?.ct_code === 'number' && hwSensor.ct_code > 0 ? hwSensor.ct_code : null;
  const raw = list.find((u) => apiId != null && hexKey(u.unit_id) === apiId) ?? null;

  const toDraft = (u: JsonObject): Draft => ({
    name: String(u.name ?? ''),
    short_name: String(u.short_name ?? ''),
    role: String(u.role ?? ''),
    phase: typeof u.phase === 'number' ? u.phase : 0,
    ratedKw: typeof u.rated_power_w === 'number' && u.rated_power_w > 0 ? String(u.rated_power_w / 1000) : '',
    maxA: typeof u.max_current_a === 'number' && u.max_current_a > 0 ? String(u.max_current_a) : '',
    group: typeof u.group_id === 'number' && u.group_id > 0 ? u.group_id : 0,
    trunk: u.role_trunk === 1 || u.role_trunk === true,
    evW: typeof u.charge_threshold_w === 'number' && u.charge_threshold_w > 0 ? String(u.charge_threshold_w) : '',
    evMin: typeof u.idle_timeout_min === 'number' && u.idle_timeout_min > 0 ? String(u.idle_timeout_min) : '',
    ct: hwCt ?? 0,
  });
  const [draft, setDraft] = useState<Draft | null>(null);
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState<{ sev: 'success' | 'error' | 'warning'; text: string } | null>(null);

  useEffect(() => {
    if (Object.keys(session.current.get('units').reported).length === 0) {
      void session.loadDomain('units').catch(() => undefined);
    }
  }, [session]);
  // Re-seed the form whenever the device's copy changes (after a save / another client).
  const rawKey = raw ? JSON.stringify(raw) : '';
  useEffect(() => { setDraft(raw ? toDraft(raw) : null); }, [rawKey, hwCt]); // eslint-disable-line react-hooks/exhaustive-deps

  // Edits not yet saved → leaving through the menu asks first (UX audit HEU-21).
  useUnsavedChanges(`unit-${apiId ?? ''}`, !!raw && !!draft && JSON.stringify(draft) !== JSON.stringify(toDraft(raw)));

  if (!raw || !draft) return null;

  const isInput = raw.role === 'MAIN_SUPPLY' && raw.type !== 'VOLTAGE_BUS';
  const isBus = raw.type === 'VOLTAGE_BUS';
  const { shortNameBytes: SHORT_NAME_BYTES, nameBytes: NAME_BYTES } = caps.limits;
  const roleLocked = SUPPLY_ROLES.has(String(raw.role));
  const roles = roleLocked
    ? [String(raw.role)]
    : (caps.roleEnum.length ? caps.roleEnum : [...UNIT_ROLE_NAMES]).filter((r) => !SUPPLY_ROLES.has(r)
      // BASIC counts consumption only: no generation / storage (export) role (root 18.10).
      && !(basicPoint && EXPORT_ROLES.has(r)));
  // Supply / bus units are the topology root, not a group member.
  const canGroup = !isInput && !isBus && !roleLocked;
  const groupOptions = [...deviceGroups.groups];
  if (draft.group > 0 && !groupOptions.some((g) => g.id === draft.group)) groupOptions.push({ id: draft.group, name: '' });
  const set = <K extends keyof Draft>(k: K, v: Draft[K]) => setDraft({ ...draft, [k]: v });
  // One name for the point (rerun 16.10 PNT-R02/PNT-20: a new «Название» never reached the tiles and
  // the flow, which show the short name). While the short name is just the name — the same, a
  // prefix of it, or empty — it follows the name (cut to the hub's byte limit); a short name the
  // user chose stays.
  const setName = (v: string) => {
    const oldName = draft.name.trim();
    const sh = draft.short_name.trim();
    const follows = sh === '' || sh === oldName || oldName.startsWith(sh);
    let next = draft.short_name;
    if (follows) {
      next = '';
      for (const ch of v.trim()) { if (utf8Len(next + ch) > SHORT_NAME_BYTES) break; next += ch; }
    }
    setDraft({ ...draft, name: v, short_name: next });
  };

  const num = (s: string): number | null => {
    if (s.trim() === '') return 0; // empty = «не задано» (0 on the wire)
    const v = Number(s.replace(',', '.'));
    return Number.isFinite(v) && v >= 0 ? v : null;
  };
  const ratedW = num(draft.ratedKw);
  const maxA = num(draft.maxA);
  // EV station's own threshold / idle (root 56be1d45): whole numbers, 0 / empty = the hub's.
  const isStation = draft.role === 'EV_CHARGING';
  const int = (s: string, max: number): number | null => { const v = num(s); return v != null && Number.isInteger(v) && v <= max ? v : null; };
  const evW = int(draft.evW, 100_000);
  const evMin = int(draft.evMin, 1440);
  const hubEv = evThresholds(sysCfg.reported.ev as unknown as EvConfig | undefined);
  const hasEvFields = raw != null && ('charge_threshold_w' in raw || 'idle_timeout_min' in raw);
  const errors: string[] = [];
  const shortBytes = utf8Len(draft.short_name.trim());
  // Only a CHANGED short name is validated — a legacy over-long one must not block other fields.
  const shortTooLong = shortBytes > SHORT_NAME_BYTES && draft.short_name.trim() !== String(raw.short_name ?? '');
  if (shortTooLong) {
    errors.push(t('unitParams.shortNameTooLong', { bytes: SHORT_NAME_BYTES, cyr: Math.floor(SHORT_NAME_BYTES / 2) }));
  }
  const nameBytes = utf8Len(draft.name.trim());
  const nameTooLong = nameBytes > NAME_BYTES && draft.name.trim() !== String(raw.name ?? '');
  if (nameTooLong) errors.push(t('unitParams.nameTooLong', { bytes: NAME_BYTES, cyr: Math.floor(NAME_BYTES / 2) }));
  // UX audit PNT-08/09/10, INP-15: say WHY Save is off; catch unit slips and limits that
  // contradict the breaker; warn (not block) on a name another point already has.
  const nameEmpty = !draft.name.trim();
  if (nameEmpty) errors.push(t('unitParams.nameEmpty'));
  const dupName = !nameEmpty && list.some((u) => hexKey(u.unit_id) !== apiId
    && String(u.name ?? '').trim().toLowerCase() === draft.name.trim().toLowerCase());
  const ratedTooBig = ratedW != null && ratedW > 1000;
  if (ratedTooBig) errors.push(t('unitParams.ratedTooBig'));
  // The input of a 3P service is three-phase even with its phase unset (root 16.10: the limit is
  // 3 × U × I of the breaker; .110 kept 11.5 kW = one phase of 3 × 50 A).
  const conductors = draft.phase === 4 || (isInput && grid.kind === '3P') ? 3 : 1;
  const breakerKw = maxA != null && maxA > 0 ? (maxA * (grid.nominalV || 230) * conductors) / 1000 : null;
  const ratedVsBreaker = ratedW != null && ratedW > 0 && breakerKw != null
    ? (ratedW > breakerKw * 1.05 ? 'above' : isInput && ratedW < breakerKw * 0.5 ? 'below' : null) : null;
  if (ratedW == null) errors.push(t('unitParams.ratedInvalid'));
  if (maxA == null) errors.push(t('unitParams.maxAInvalid'));
  if (isStation && hasEvFields && evW == null) errors.push(t('unitParams.evWInvalid'));
  if (isStation && hasEvFields && evMin == null) errors.push(t('unitParams.evMinInvalid'));

  const next: JsonObject = {
    ...raw,
    name: draft.name.trim() || raw.name || '',
    short_name: draft.short_name.trim(),
    phase: draft.phase,
    rated_power_w: Math.round((ratedW ?? 0) * 1000),
    max_current_a: maxA ?? 0,
    ...(roleLocked ? {} : { role: draft.role }),
    // G5 membership (root e139ab5e: per-unit patch accepts both, live + persisted).
    ...(canGroup ? { group_id: draft.group, role_trunk: draft.group > 0 && draft.trunk ? 1 : 0 } : {}),
    ...(isStation && hasEvFields ? { charge_threshold_w: evW ?? 0, idle_timeout_min: evMin ?? 0 } : {}),
  };
  // The CT rating is a LIMIT (operator 18.10): a breaker above the sensor's accurate range clips.
  const ctRangeOf = (code: number) => {
    const rating = ctRatingA(code, { ...caps.ctModelsFixed, ...caps.ctModels });
    if (!rating) return null;
    // Only the fitted CT has a limit — the hub's clipping start (ct_fs_a); another model picked here
    // gets its figure from the hub once saved, so no guess before that.
    const r = code === hwCt ? channelRange(hwSensor, ctRatingOf(hwSensor, caps.ctModels) ?? rating, sensorCls) : null;
    return r ? { lo: nf(r.lo, 2), hi: nf(r.hi, 1), hiA: r.hi } : null;
  };
  const ctNow = ctRangeOf(draft.ct || hwCt || 0);
  const ctOverLimit = ctNow != null && maxA != null && maxA > ctNow.hiA;
  const patch: JsonObject = {};
  const ctChanged = hwCt != null && draft.ct > 0 && draft.ct !== hwCt;
  if (ctChanged) patch.ct_code = draft.ct;
  for (const k of ['name', 'short_name', 'phase', 'rated_power_w', 'max_current_a', 'role', 'group_id', 'role_trunk', 'charge_threshold_w', 'idle_timeout_min']) {
    // group_id / role_trunk absent on the hub's copy = 0 (an untouched form sends nothing).
    const was = raw[k] ?? (k === 'group_id' || k === 'role_trunk' ? 0 : undefined);
    if (next[k] !== undefined && next[k] !== (k === 'role_trunk' && was === true ? 1 : was)) patch[k] = next[k];
  }
  const dirty = Object.keys(patch).length > 0;
  const roleChanged = 'role' in patch;

  const save = async () => {
    setBusy(true);
    setMsg(null);
    try {
      const newUnits = list.map((u) => (u === raw ? next : u));
      const full = JSON.stringify({ type: 'units_config', data: { ...units.reported, units: newUnits } });
      if (ctChanged) markModuleRestart(hwSensor?.module); // the module restarts on a new CT model
      await session.patchUnit(apiId!, patch, newUnits, () => {
        // Only reached on firmware without the per-unit patch: guard the full-array body size.
        if (full.length > MAX_FULL_BODY) {
          throw Object.assign(new Error(t('unitParams.fullListTooBig', { size: nf(full.length / 1024, 1), max: MAX_FULL_BODY / 1000 })), { local: true });
        }
      });
      if (ctChanged) await refreshHardware(); // the CT model is read back from /api/v1/sensors
      setMsg({
        sev: 'success',
        text: roleChanged
          ? savedText('unitParams.savedRoleChanged')
          : savedText('unitParams.saved'),
      });
    } catch (e) {
      const err = e as { status?: number; message?: string; data?: { message?: string; error?: string }; local?: boolean };
      setMsg({
        sev: 'error',
        text: err.status === 401 || err.status === 403 ? t('common.adminTokenRequired')
          : err.status === 409 && 'ct_code' in patch ? t('unitParams.ctFixedConflict')
          : err.status === 409 ? t('unitParams.roleLockedConflict')
          : err.status === 400 && 'ct_code' in patch ? t('unitParams.ctRejected', { message: err.data?.message ?? err.data?.error ?? '' })
          : err.local ? t('unitParams.notSaved', { message: err.message })
          : err.data?.message || err.data?.error
            ? t('common.deviceRejectedWith', { message: err.data?.message ?? err.data?.error })
            : t('common.deviceRejected'),
      });
    } finally {
      setBusy(false);
    }
  };

  return (
    <Card>
      <Stack spacing={1.5}>
        <Typography sx={{ fontWeight: 600, fontSize: 16 }}>{t('unitParams.title')}</Typography>
        <Box sx={{ display: 'grid', gap: 1.5, gridTemplateColumns: { xs: '1fr', sm: '1fr 1fr' } }}>
          <TextField
            size="small" label={t('unitParams.name')} value={draft.name} onChange={(e) => setName(e.target.value)}
            helperText={nameEmpty ? t('unitParams.nameEmpty') : dupName ? t('unitParams.nameDuplicate') : t('unitParams.bytesUsed', { used: nameBytes, max: NAME_BYTES })}
            error={nameTooLong || nameEmpty}
          />
          <TextField
            size="small" label={t('unitParams.shortName')} value={draft.short_name}
            onChange={(e) => set('short_name', e.target.value)}
            helperText={t('unitParams.shortNameHelper', { used: shortBytes, max: SHORT_NAME_BYTES })}
            error={shortTooLong}
          />
          {!isBus && (
            <TextField
              select size="small" label={t('unitParams.role')} value={draft.role} disabled={roleLocked}
              SelectProps={{ renderValue: (v) => roleLabel(String(v)) }}
              onChange={(e) => set('role', e.target.value)}
              helperText={<>{roleLocked ? t('unitParams.roleLockedHelper') : t('unitParams.roleHelper')}<HelpTip topic="role" /></>}
            >
              {/* One line of meaning per role (UX audit PNT-11: «Прибор» / «Нагрузка» / «Умный прибор»
                  side by side with nothing to tell them apart). */}
              {roles.map((r) => (
                <MenuItem key={r} value={r} sx={{ display: 'block' }}>
                  <Typography sx={{ fontSize: 14 }}>{roleLabel(r)}</Typography>
                  <Typography sx={{ fontSize: 12 }} color="text.secondary">{tCode('roleHint', r)}</Typography>
                </MenuItem>
              ))}
            </TextField>
          )}
          <TextField
            select size="small" label={t('unitParams.phase')} value={draft.phase} onChange={(e) => set('phase', Number(e.target.value))}
            helperText={<>{grid.fromDevice ? t('unitParams.phaseHelperGrid', { grid: gridText(grid) }) : ''}<HelpTip topic="phase" /></>}
          >
            {/* Codes no hardware-validated system offers say so in the label itself (F20). */}
            {phaseOptions(grid, draft.phase).map((v) => <MenuItem key={v} value={v}>{PHASE_LABEL[v] ? t(PHASE_LABEL[v]) : String(v)}{phaseCodeValidated(v, validated) ? '' : t('grid.notValidated')}</MenuItem>)}
          </TextField>
          {!isBus && (
            <TextField
              size="small" label={isInput ? t('unitParams.inputLimit', { unit: t('units.kW') }) : t('unitParams.lineRating', { unit: t('units.kW') })} value={draft.ratedKw}
              onChange={(e) => set('ratedKw', e.target.value)} inputProps={{ inputMode: 'decimal' }}
              helperText={ratedTooBig ? t('unitParams.ratedTooBig')
                : ratedVsBreaker && breakerKw != null ? <>
                  {t(ratedVsBreaker === 'above' ? 'unitParams.ratedAboveBreaker' : 'unitParams.ratedBelowBreaker', { kw: breakerKw.toLocaleString(intlLocale(), { maximumFractionDigits: 1 }) })}
                  {' '}<Link component="button" type="button" underline="hover" onClick={() => set('ratedKw', breakerKw.toLocaleString(intlLocale(), { maximumFractionDigits: 1, useGrouping: false }))}>
                    {t('unitParams.useBreakerKw', { kw: breakerKw.toLocaleString(intlLocale(), { maximumFractionDigits: 1 }), n: conductors, v: grid.nominalV || 230, a: maxA ?? '' })}
                  </Link>
                </>
                  : isInput ? t('unitParams.inputLimitHelper') : t('unitParams.lineRatingHelper')}
              error={ratedW == null || ratedTooBig}
            />
          )}
          {!isBus && (
            <TextField
              size="small" label={isInput ? t('unitParams.inputBreaker', { unit: t('units.A') }) : t('unitParams.lineBreaker', { unit: t('units.A') })} value={draft.maxA}
              onChange={(e) => set('maxA', e.target.value)} inputProps={{ inputMode: 'decimal' }}
              helperText={t('unitParams.breakerHelper')} error={maxA == null}
            />
          )}
          {/* SCT- channel: the user's CT, chosen here; CT- channel: factory-paired, shown only. */}
          {hwCt != null && ctFree && Object.keys(caps.ctModels).length > 0 && (
            <TextField
              select size="small" label={t('unitParams.ct')} value={draft.ct}
              onChange={(e) => set('ct', Number(e.target.value))}
              helperText={<>{ctRangeOf(draft.ct) ? `${t('unitParams.ctRangeNow', ctRangeOf(draft.ct)!)} ` : ''}{t('unitParams.ctHelper')}</>}
            >
              {ctOptions(caps.ctModels, hwCt).map(([code, name]) => (
                <MenuItem key={code} value={code}>{name}{ctRangeOf(code) ? ` · ${t('unitParams.ctRangeShort', ctRangeOf(code)!)}` : ''}</MenuItem>
              ))}
            </TextField>
          )}
          {hwCt != null && sensorCls === 'uncalibrated' && (
            <TextField size="small" label={t('unitParams.ct')} value={t('unitParams.ctUncalibrated')} InputProps={{ readOnly: true }}
              helperText={t('unitParams.ctUncalibratedHelper')} />
          )}
          {hwCt != null && !ctFree && sensorCls !== 'uncalibrated' && (
            <TextField size="small" label={t('unitParams.ct')} value={ctName(hwCt, hwPoint.module, caps.ctModels, caps.ctModelsFixed)}
              InputProps={{ readOnly: true }} helperText={t('unitParams.ctFixed')} />
          )}
          {isStation && hasEvFields && (
            <TextField
              size="small" label={t('unitParams.evW')} value={draft.evW} placeholder={String(hubEv.chargeThresholdW)}
              onChange={(e) => set('evW', e.target.value)} inputProps={{ inputMode: 'numeric' }} error={evW == null}
              helperText={t('unitParams.evHubHelper', { value: `${hubEv.chargeThresholdW} ${t('units.W')}` })}
            />
          )}
          {isStation && hasEvFields && (
            <TextField
              size="small" label={t('unitParams.evMin')} value={draft.evMin} placeholder={String(hubEv.idleTimeoutMin)}
              onChange={(e) => set('evMin', e.target.value)} inputProps={{ inputMode: 'numeric' }} error={evMin == null}
              helperText={t('unitParams.evHubHelper', { value: t('unitParams.minutes', { n: hubEv.idleTimeoutMin }) })}
            />
          )}
          {canGroup && (
            <TextField
              select size="small" label={t('unitParams.group')} value={draft.group}
              onChange={(e) => setDraft({ ...draft, group: Number(e.target.value), trunk: Number(e.target.value) > 0 && draft.trunk })}
              helperText={groupOptions.length ? t('unitParams.groupHelper') : t('unitParams.groupHelperEmpty')}
            >
              <MenuItem value={0}>{t('unitParams.groupNone')}</MenuItem>
              {groupOptions.map((g) => <MenuItem key={g.id} value={g.id}>{g.name || t('flow.groupN', { n: g.id })}</MenuItem>)}
            </TextField>
          )}
          {canGroup && draft.group > 0 && (
            <FormControlLabel
              control={<Checkbox size="small" checked={draft.trunk} onChange={(e) => set('trunk', e.target.checked)} />}
              label={<Box><Typography sx={{ fontSize: 14 }}>{t('unitParams.trunk')}</Typography><Typography sx={{ fontSize: 12 }} color="text.secondary">{t('unitParams.trunkHelper')}</Typography></Box>}
            />
          )}
        </Box>

        {errors.map((e) => <Alert key={e} severity="warning">{e}</Alert>)}
        {/* Writing a model replaces the channel's factory calibration (made with the fitted CT) by the
            type's — no way back (root 18.10, UI7 on .139). */}
        {ctChanged && <Alert severity="warning">{t('unitParams.ctWriteWarn')}</Alert>}
        {ctOverLimit && <Alert severity="warning">{t(isInput ? 'unitParams.ctOverLimitInput' : 'unitParams.ctOverLimit', { a: nf(maxA ?? 0, 0), hi: ctNow!.hi })}</Alert>}
        {msg && <Alert severity={msg.sev}>{msg.text}</Alert>}

        {/* The form's save bar sticks to the bottom while there are edits (HEU-R01: the buttons
            scrolled away under a long form); it says what state the edit is in. */}
        <Stack direction="row" spacing={1} alignItems="center" sx={dirty ? { position: 'sticky', bottom: 0, zIndex: 5, py: 1, bgcolor: 'background.paper', borderTop: 1, borderColor: 'divider' } : undefined}>
          <Button variant="contained" disabled={!dirty || busy || errors.length > 0} onClick={save}>
            {busy ? t('common.saving') : t('common.save')}
          </Button>
          <Button disabled={!dirty || busy} onClick={() => { setDraft(toDraft(raw)); setMsg(null); }}>{t('common.cancel')}</Button>
          {dirty && <Typography sx={{ fontSize: 13 }} color={errors.length ? 'error' : 'text.secondary'}>{errors.length ? t('form.fixErrors') : t('unitParams.unsaved')}</Typography>}
        </Stack>
      </Stack>
    </Card>
  );
};
