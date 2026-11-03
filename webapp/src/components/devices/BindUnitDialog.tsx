/**
 * BindUnitDialog — create/bind a unit from an unbound sensor (POST /api/v1/units). A module's
 * phase group (`ch: 254`, entities/phaseGroup) binds all its current channels as one 3P unit.
 */
import React, { useEffect, useState } from 'react';
import {
  Dialog, DialogTitle, DialogContent, DialogActions, Button, Stack, Alert, Typography, TextField, MenuItem, Chip,
} from '@mui/material';
import { useDeviceSession, useConfigShadow } from '../../core';
import { useUnitsConfig } from '../../hooks/useUnitsConfig';
import { HISTORY_SLOTS } from '../common/NoHistoryNote';
import { useGridSystem } from '../../hooks/useGridSystem';
import { PHASE_LABEL, phaseOptions } from './UnitParamsPanel';
import { parseDecimal } from '../common/DecimalField';
import { FormField } from '@rbgrid/design-system';
import { getApiClient } from '../../services/api';
import { SensorListEntry, CreateUnitRequest, CreateUnitSensorRef } from '../../types/sensors';
import { UNIT_ROLE_NAMES, UnitRoleName } from '../../constants/enums';
import { isPhaseGroup, channelLabel } from '../../entities/phaseGroup';
import { useHardware } from '../../hooks/useHardware';
import { isBasic } from '../../entities/moduleLine';
import { t, tCode } from '../../i18n';

function sensorRef(s: SensorListEntry): CreateUnitSensorRef {
  return { iface: 'i2c', bus: s.bus, module: s.module, ch: s.ch };
}

function sensorLabel(s: SensorListEntry): string {
  return t('bind.addrI2c', { module: s.module, ch: channelLabel(s.ch, s.ch_label), bus: s.bus })
    + (s.addr ? ` · 0x${s.addr.toString(16)}` : '');
}

/** A point slug the hub accepts (T7). */
const SLUG = /^[a-z0-9_.-]{1,32}$/;

/** Supply roles are topology — the input is set up in the wizard, not bound from the list
 *  (the point page locks them too; UX audit PNT-06). */
const BINDABLE_ROLES = UNIT_ROLE_NAMES.filter((r) => r !== 'MAIN_SUPPLY' && r !== 'ALTERNATIVE_SUPPLY');

export const BindUnitDialog: React.FC<{
  sensor: SensorListEntry | null;
  onClose: () => void;
  onBound: () => void;
}> = ({ sensor, onClose, onBound }) => {
  const [name, setName] = useState('');
  const [role, setRole] = useState<UnitRoleName>('DEVICE_LOAD');
  const [location, setLocation] = useState('');
  // Phase and nominal right here, not «bind, then go find the point page» (UX audit PNT-06).
  const [phase, setPhase] = useState(0);
  const [ratedKw, setRatedKw] = useState('');
  const session = useDeviceSession();
  const unitsJoin = useUnitsConfig();
  const grid = useGridSystem(unitsJoin);
  // A BASIC channel counts consumption only: no generation / storage role (root 18.10).
  const { modules } = useHardware();
  const basic = isBasic(modules.find((m) => m.module === sensor?.module));
  const roleChoices = BINDABLE_ROLES.filter((r) => !(basic && (r === 'RENEWABLE_SOURCE' || r === 'STORAGE_SYSTEM')));
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  // Point slug (T7): empty → the hub derives it from the name. A removed point's slug can be
  // revived (`reuse`) — the ID only: the hub deleted its history on unbind (root 18.10), HA/MQTT
  // continue theirs by the ID. Offered from the hub's tombstones, or after a 409 POINT_TOMBSTONED.
  const [point, setPoint] = useState('');
  const [tombHit, setTombHit] = useState<string | null>(null);
  const unitsShadow = useConfigShadow('units');
  const tombstones = (Array.isArray(unitsShadow.reported.point_tombstones) ? unitsShadow.reported.point_tombstones : [])
    .filter((x): x is string => typeof x === 'string');
  useEffect(() => {
    // Fresh tombstones on open: a point unbound a minute ago is in the list.
    if (sensor) session.loadDomain('units').catch(() => { /* the 409 path still offers reuse */ });
  }, [sensor, session]);
  const slug = point.trim().toLowerCase();
  const slugBad = slug !== '' && !SLUG.test(slug);
  const reuse = slug !== '' && (tombstones.includes(slug) || tombHit === slug);

  const submit = async () => {
    if (!sensor || !name.trim() || slugBad) return;
    setBusy(true);
    setError(null);
    const body: CreateUnitRequest = {
      sensor: sensorRef(sensor),
      name: name.trim(),
      role,
      // No «History storage» choice: the hub rolls days / weeks / months / years up from the
      // hourly records for every point whatever stat_class says, in a fixed-size slot (root 02.11).
      stat_class: '1HOUR',
      location: location.trim() || undefined,
      ...(slug ? { point: slug } : {}),
      ...(reuse ? { reuse: true } : {}),
    };
    try {
      const res = await getApiClient().createUnit(body);
      if (res.status === 'ok') {
        const rated = parseDecimal(ratedKw);
        const patch: Record<string, number> = {};
        if (phase && !isPhaseGroup(sensor)) patch.phase = phase;
        if (rated != null && !Number.isNaN(rated) && rated > 0) patch.rated_power_w = Math.round(rated * 1000);
        if (res.unit_id && Object.keys(patch).length) {
          try { await session.transport.patchUnit(res.unit_id.replace(/^0x/i, '').toUpperCase().padStart(4, '0'), patch); }
          catch { /* the point exists; phase/nominal can be set on its page */ }
        }
        onBound();
      } else if (res.code === 'POINT_TOMBSTONED' && slug) {
        setTombHit(slug); // the warning shows; the next press sends reuse
      } else if (res.code === 'POINT_TAKEN') {
        setError(t('bind.pointTaken', { point: slug }));
      } else {
        setError(res.message ?? t('bind.failed'));
      }
    } catch {
      setError(t('bind.requestFailed'));
    } finally {
      setBusy(false);
    }
  };

  return (
    <Dialog open={!!sensor} onClose={busy ? undefined : onClose} fullWidth maxWidth="xs">
      <DialogTitle>{t('bind.title')}</DialogTitle>
      <DialogContent>
        <Stack spacing={2} sx={{ mt: 0.5 }}>
          {sensor && (
            <Typography variant="caption" color="text.secondary">
              {sensorLabel(sensor)}
            </Typography>
          )}
          {sensor && isPhaseGroup(sensor) && <Alert severity="info">{t('bind.phaseGroupHint')}</Alert>}
          {/* v1.1.2: every history slot is taken — the new point will be live only (root 6e32d29). */}
          {unitsJoin.historyCount != null && unitsJoin.historyCount >= HISTORY_SLOTS && <Alert severity="info">{t('bind.liveOnly', { n: HISTORY_SLOTS })}</Alert>}
          {error && <Alert severity="error">{error}</Alert>}
          <FormField label={t('bind.name')} value={name} onChange={setName} required />
          <TextField
            size="small" label={t('bind.point')} value={point} onChange={(e) => setPoint(e.target.value)}
            error={slugBad} helperText={slugBad ? t('bind.pointBad') : t('bind.pointHelper')}
            inputProps={{ autoCapitalize: 'none', spellCheck: false }}
          />
          {tombstones.length > 0 && (
            <Stack spacing={0.5}>
              <Typography variant="caption" color="text.secondary">{t('bind.tombstones')}</Typography>
              <Stack direction="row" flexWrap="wrap" useFlexGap gap={0.5}>
                {tombstones.map((p) => (
                  <Chip key={p} size="small" label={p} variant={slug === p ? 'filled' : 'outlined'} color={slug === p ? 'primary' : 'default'}
                    onClick={() => setPoint(p)} />
                ))}
              </Stack>
            </Stack>
          )}
          {reuse && <Alert severity="warning">{t('bind.reuseWarn', { point: slug })}</Alert>}
          <TextField
            select size="small" label={t('bind.role')} value={role} onChange={(e) => setRole(e.target.value as UnitRoleName)}
            SelectProps={{ renderValue: (v) => tCode('role', String(v)) }}
          >
            {roleChoices.map((r) => (
              <MenuItem key={r} value={r} sx={{ display: 'block' }}>
                <Typography sx={{ fontSize: 14 }}>{tCode('role', r)}</Typography>
                <Typography sx={{ fontSize: 12 }} color="text.secondary">{tCode('roleHint', r)}</Typography>
              </MenuItem>
            ))}
          </TextField>
          {sensor && !isPhaseGroup(sensor) && (
            <TextField select size="small" label={t('unitParams.phase')} value={phase} onChange={(e) => setPhase(Number(e.target.value))}>
              {phaseOptions(grid, phase).map((v) => <MenuItem key={v} value={v}>{PHASE_LABEL[v] ? t(PHASE_LABEL[v]) : String(v)}</MenuItem>)}
            </TextField>
          )}
          <TextField
            size="small" label={t('unitParams.lineRating', { unit: t('units.kW') })} value={ratedKw}
            onChange={(e) => setRatedKw(e.target.value)} inputProps={{ inputMode: 'decimal' }}
            helperText={t('bind.ratedHelper')}
          />
          <FormField label={t('bind.location')} value={location} onChange={setLocation} />
          <Typography variant="caption" color="text.secondary">
            {t('bind.haAfterReboot')}
          </Typography>
        </Stack>
      </DialogContent>
      <DialogActions>
        <Button onClick={onClose} disabled={busy}>{t('bind.cancel')}</Button>
        <Button onClick={submit} variant="contained" disabled={busy || !name.trim() || slugBad}>
          {busy ? t('bind.binding') : reuse ? t('bind.bindReuse') : t('bind.bind')}
        </Button>
      </DialogActions>
    </Dialog>
  );
};
