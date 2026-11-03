/**
 * SettingsReconcileBar — the shadow reconciliation control. Reads the whole DeviceShadow
 * and shows the pending diff ("N changes · M need reboot"), with Apply-all and Reset-all.
 * This is the Device-Shadow desired→reported convergence made visible: edit freely across
 * domains, see the delta, apply once.
 */
import React, { useState } from 'react';
import { Box, Stack, Typography, Button, Chip, Alert, CircularProgress } from '@mui/material';
import { useDeviceSession, ConfigDomain, UnsupportedDomainError } from '../../core';
import { ApplyAllError } from '../../core/session/DeviceSession';
import { getApiClient } from '../../services/api';
import { t } from '../../i18n';
import { savedText } from '../../services/savedText';
import { useUnsavedChanges } from '../../app/unsavedGuard';
import { useFieldErrorCount, clearFieldErrors } from '../../app/fieldErrors';
import { ApiRequestError } from '../../services/api';
import { humanize } from './ShadowConfigEditor';
import { useConfirm } from '../common/ConfirmDialog';

/** Domain → phrase key (resolved at render). */
const DOMAIN_LABEL: Partial<Record<ConfigDomain, string>> = {
  system: 'reconcile.domainSystem', time: 'reconcile.domainTime', hardware: 'reconcile.domainHardware', telemetry: 'reconcile.domainTelemetry',
  tariff: 'reconcile.domainTariff', units: 'reconcile.domainUnits', mqtt: 'reconcile.domainMqtt', wifi: 'reconcile.domainWifi',
};

export const SettingsReconcileBar: React.FC = () => {
  const session = useDeviceSession();
  const shadow = session.current;
  const dirty = shadow.dirtyDomains;
  // Two ways to save (operator 18.10): what the hub applies at once goes with «Применить»;
  // what it reads only at start goes with «Сохранить и перезагрузить», never silently together.
  const split = session.pendingSplit();
  const [applying, setApplying] = useState(false);
  const [msg, setMsg] = useState<{ sev: 'success' | 'error' | 'warning'; text: string } | null>(null);
  // A reboot-domain write is stored but only takes effect after a restart; nothing reboots
  // on its own (review 02 B-8) → offer it explicitly instead of claiming it happens.
  const [rebootPending, setRebootPending] = useState(false);
  const [rebooting, setRebooting] = useState(false);
  const fieldErrors = useFieldErrorCount();
  const confirm = useConfirm();

  const apply = async () => {
    setApplying(true);
    setMsg(null);
    try {
      const jobs = await session.applyLive();
      // The hub's answer to each write decides: a «live» write it still wants a restart for
      // is said so, not hidden.
      if (session.awaitingReboot(jobs)) {
        setRebootPending(true);
        setMsg({ sev: 'warning', text: savedText('reconcile.writtenNeedsReboot') });
      } else {
        setMsg({ sev: 'success', text: savedText('reconcile.applied') });
      }
    } catch (err) {
      showError(err);
    } finally {
      setApplying(false);
    }
  };

  const saveAndReboot = async () => {
    const names = split.reboot.map((d) => t(DOMAIN_LABEL[d] ?? d)).join(', ');
    if (!(await confirm({
      title: t('reconcile.rebootSaveTitle'), body: t('reconcile.rebootSaveBody', { list: names }),
      confirmLabel: t('reconcile.saveAndReboot'),
    }))) return;
    setApplying(true);
    setMsg(null);
    try {
      await session.applyRebootPart();
    } catch (err) {
      showError(err);
      setApplying(false);
      return;
    }
    setApplying(false);
    setRebootPending(true);
    await reboot();
  };

  const showError = (err: unknown) => {
    // Each domain applies on its own (W191-02): name the ones that didn't, and why.
    let e: unknown = err;
    if (err instanceof ApplyAllError) {
      const why = (f: { domain: ConfigDomain; error: unknown }) => (f.error instanceof UnsupportedDomainError
        ? t(f.domain === 'time' ? 'reconcile.timeOwnPanel' : 'reconcile.domainReadOnly')
        : t('reconcile.domainRefused'));
      const list = err.failures.map((f) => `${t(DOMAIN_LABEL[f.domain] ?? f.domain)} — ${why(f)}`).join('; ');
      if (err.applied.length || err.failures.every((f) => f.error instanceof UnsupportedDomainError)) {
        setMsg({ sev: 'warning', text: t('reconcile.partlyApplied', { list }) });
        return;
      }
      e = err.failures[0].error;
    }
    // The hub names the field it refused (canon 2d2653ee: 400 OUT_OF_RANGE + `field` path).
    const data = e instanceof ApiRequestError ? (e.data as Record<string, unknown> | undefined) : undefined;
    const field = typeof data?.field === 'string' ? data.field : null;
    setMsg({ sev: 'error', text: e instanceof ApiRequestError && e.code === 'OUT_OF_RANGE' && field
      ? t('reconcile.outOfRange', { field: humanize(field.split('.').pop() ?? field) })
      : t('reconcile.applyFailed') });
  };

  const reboot = async () => {
    setRebooting(true);
    try {
      await getApiClient().rebootDevice();
      setRebootPending(false);
      setMsg({ sev: 'success', text: t('reconcile.rebooting') });
    } catch {
      setMsg({ sev: 'error', text: t('reconcile.rebootFailed') });
    } finally {
      setRebooting(false);
    }
  };

  const reset = () => {
    dirty.forEach((d) => session.resetDomain(d));
    clearFieldErrors();
    setMsg(null);
  };
  // Settings drafts live in the device shadow (they survive the page) — leaving asks, and a
  // confirmed leave drops them, so nothing lingers silently (HEU-21). Written-awaiting-reboot
  // edits are not «unsaved».
  useUnsavedChanges('settings', dirty.length > 0 && !rebootPending, reset);

  return (
    <Box
      sx={{
        position: 'sticky', top: 0, zIndex: 5, mb: 2, p: 1.5, borderRadius: 3,
        bgcolor: 'background.paper', border: 1, borderColor: dirty.length ? 'primary.main' : 'divider',
      }}
    >
      <Stack direction="row" alignItems="center" spacing={1.5} flexWrap="wrap" useFlexGap>
        <Typography variant="subtitle2" fontWeight={700}>
          {/* After a write that needs a reboot the delta stays until the restart — it is written,
              not unsaved (HEU-06: «Записано» next to «Несохранённых изменений: 1»). */}
          {rebootPending && dirty.length ? t('reconcile.writtenPending', { n: dirty.length })
            : dirty.length ? t('reconcile.unsavedCount', { n: dirty.length })
              // A red field is an edit too, just not an acceptable one (HEU-R04: «Нет несохранённых
              // изменений» next to a field in error).
              : fieldErrors > 0 ? t('reconcile.invalidEdit') : t('reconcile.noUnsaved')}
        </Typography>
        {split.live.map((d) => <Chip key={`l-${d}`} size="small" label={t(DOMAIN_LABEL[d] ?? d)} variant="outlined" />)}
        {split.reboot.map((d) => (
          <Chip key={`r-${d}`} size="small" color="warning" variant="outlined"
            label={t('reconcile.afterReboot', { domain: t(DOMAIN_LABEL[d] ?? d) })} />
        ))}
        {fieldErrors > 0 && <Typography variant="caption" color="error.main">{t('form.fixErrors')}</Typography>}
        <Box sx={{ ml: 'auto', width: { xs: '100%', sm: 'auto' } }}>
          <Stack direction="row" spacing={1} justifyContent="flex-end">
            <Button size="small" onClick={reset} disabled={(!dirty.length && !fieldErrors) || applying}>{t('reconcile.reset')}</Button>
            {split.reboot.length > 0 && !rebootPending && (
              <Button size="small" variant="outlined" color="warning" onClick={saveAndReboot}
                disabled={applying || fieldErrors > 0}>
                {t('reconcile.saveAndReboot')}
              </Button>
            )}
            <Button
              size="small" variant="contained" onClick={apply} disabled={!split.live.length || applying || fieldErrors > 0}
              startIcon={applying ? <CircularProgress size={16} aria-label={t('common.loading')} /> : undefined}
            >
              {applying ? t('reconcile.applying') : t('reconcile.apply')}
            </Button>
          </Stack>
        </Box>
      </Stack>
      {msg && (
        <Alert
          severity={msg.sev}
          sx={{ mt: 1.5 }}
          action={rebootPending ? (
            <Button color="inherit" size="small" onClick={reboot} disabled={rebooting}>
              {rebooting ? t('reconcile.rebootingShort') : t('reconcile.rebootNow')}
            </Button>
          ) : undefined}
        >
          {msg.text}
        </Alert>
      )}
    </Box>
  );
};
