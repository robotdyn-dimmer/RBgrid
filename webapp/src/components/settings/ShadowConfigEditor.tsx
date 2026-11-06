/**
 * ShadowConfigEditor — a generic, data-driven editor for one config domain bound to the
 * ConfigShadow (desired/reported/delta). It renders whatever shape the device reports —
 * primitives become FormField/ToggleRow editors, nested objects recurse — so it adapts to
 * any domain without hardcoded field names (the HA/Azure schema-driven-form pattern).
 * Editing mutates `desired`; the global SettingsReconcileBar applies the delta.
 *
 * Arrays / deeply structured values (e.g. tariff zones) are shown read-only for now — those
 * keep their bespoke editors (TariffsPage). [Follow-up: verify v1 config shapes on the bench.]
 */
import React, { useEffect, useRef } from 'react';
import { Box, Typography, Stack, CircularProgress, MenuItem, TextField } from '@mui/material';
import { FormField, ToggleRow, SettingRow } from '@rbgrid/design-system';
import { useCapabilities, useConfigShadow, useDeviceSession, ConfigDomain, JsonObject, JsonValue } from '../../core';
import { t } from '../../i18n';
import { DecimalField } from '../common/DecimalField';
import { numberRules } from '../../core/schema/fieldRules';
import { DeviceLimits } from '../../core/schema/capabilities';
import { SYSTEM_UNUSED_FIELDS } from '../../core/shadow/ConfigShadow';
import { EV_DEFAULTS } from '../stations/evStatus';
import { setFieldError, clearFieldErrors, useFieldEpoch } from '../../app/fieldErrors';
import { RELEASE_HIDDEN } from '../../constants/release';

/**
 * Labels for the config keys the hub reports (system / hardware / telemetry / units / mqtt,
 * verified on .139) live in the phrase files as configEditor.field_<key>; unknown keys fall
 * back to a humanized key so new firmware fields still render.
 */
export const humanize = (key: string): string => {
  const phrase = `configEditor.field_${key}`;
  const label = t(phrase);
  return label !== phrase ? label : key.replace(/_/g, ' ').replace(/^\w/, (c) => c.toUpperCase());
};

/** Immutably set a value at a key-path inside a JSON object. */
function setAtPath(obj: JsonObject, path: string[], value: JsonValue): JsonObject {
  const [head, ...rest] = path;
  if (rest.length === 0) return { ...obj, [head]: value };
  const child = obj[head];
  const base = child && typeof child === 'object' && !Array.isArray(child) ? (child as JsonObject) : {};
  return { ...obj, [head]: setAtPath(base, rest, value) };
}

/** Keys with a dedicated panel on the same page (not repeated in the generic editor). */
// language: «Интерфейс» (the interface language IS the hub's, operator 18.10).
const DEDICATED = new Set(['site_profile', 'groups', 'grid', 'language', 'ev']); // ev: «Зарядные станции»
/** Envelope / transport metadata that leaks into a flattened config — never a setting
 *  (UX audit HEU-07: «Frame», «Ws clients», «List» showed up as fields). */
// enable_dst: the hub takes DST from the TZ rule alone, the flag changes nothing (content dst-toggle-dead).
const HIDDEN = new Set(['type', 'version', 'device_id', 'timestamp_local', 'timestamp_ntp', 'frame', 'ws_clients', 'list', 'enable_dst']);
/** Keys of features outside the release scope (constants/release): ESP-NOW (esp_now_*, espnow*). */
const outOfScope = (name: string): boolean => RELEASE_HIDDEN.espnow && /^esp_?now/i.test(name);
/** Device STATE reported next to the settings — shown as text, never as an editable field
 *  (HEU-07/PNT-15: «Всего точек», «Система исправна», «Подключено» were inputs/switches). */
const STATUS_KEYS = new Set(['units_count', 'active_units_count', 'system_healthy', 'persisted', 'connected', 'current_status', 'active_source']);
const isStatusKey = (k: string): boolean =>
  STATUS_KEYS.has(k) || /(_count|_syncs|_requests_sent|_responses_received|_activations|_us|_ms_avg)$/.test(k) || /^(last_|average_|max_observed_)/.test(k);
/** Enum fields where "" is a valid «not set» value (firmware semantics, root cfa380bd). */
const UNSET_ALLOWED = new Set(['language']);

/** Option label: phrase configEditor.opt_<field>_<VALUE> → the value itself. */
const optLabel = (field: string, v: string): string => {
  const k = `configEditor.opt_${field}_${v || 'unset'}`;
  const s = t(k, { w: EV_DEFAULTS.chargeThresholdW, min: EV_DEFAULTS.idleTimeoutMin });
  return s !== k ? s : v;
};

/** What «0» means for this field (EV: the app's default; nominals: the hub's), else generic. */
function zeroHelp(name: string): string {
  const k = `configEditor.zero_${name}`;
  const s = t(k);
  return s !== k ? s : t('configEditor.zeroUnset');
}

const Field: React.FC<{
  name: string;
  value: JsonValue;
  path: string[];
  /** Absent → read-only (domain has no working POST on the device). */
  onChange?: (path: string[], value: JsonValue) => void;
  enums: Record<string, string[]>;
  limits?: DeviceLimits;
  /** Error-registry prefix (the domain). */
  errId: string;
}> = ({ name, value, path, onChange, enums, limits, errId }) => {
  const label = humanize(name);
  const options = enums[name];
  if (HIDDEN.has(name) || outOfScope(name)) return null;
  // Arrays (zones, units, channels) have their own pages — no «[элементов: N]» stub here.
  if (Array.isArray(value)) return null;

  if ((!onChange || isStatusKey(name)) && (typeof value !== 'object' || value === null)) {
    const shown = typeof value === 'boolean' ? (value ? t('configEditor.on') : t('configEditor.off')) : value == null || value === '' ? '—' : String(value);
    return <SettingRow label={label} value={shown} />;
  }

  if (typeof value === 'boolean') {
    return <ToggleRow label={label} checked={value} onChange={(v) => onChange!(path, v)} />;
  }
  if (typeof value === 'number') {
    // Range / required / RU comma checked inline; an invalid text never reaches the device
    // (UX audit W1-7: −50 W threshold, empty → 0) and blocks «Применить» while it stands.
    const id = `${errId}.${path.join('.')}`;
    const { readOnly, ...rules } = numberRules(name, limits, errId);
    // A mirror the hub ignores on POST (system.grid.nominal_*, json_buffer_size) — shown, not edited.
    if (readOnly) return <SettingRow label={label} value={value === 0 && rules.allowZero ? '—' : String(value)} />;
    return (
      // 0 = «not set» (hub zero_unset): say what that means instead of a bare «0 V» next to the
      // grid's «230 V» (fresh hub W191-17).
      <DecimalField label={label} value={value} {...rules}
        helper={value === 0 && rules.allowZero ? zeroHelp(name) : undefined}
        onChange={(v, err) => { setFieldError(id, err); if (v != null && !err) onChange!(path, v); }} />
    );
  }
  if (typeof value === 'string' && options && options.length > 0) {
    // Schema enum → select (device values are strict; an unknown current value stays visible).
    const opts = [...(UNSET_ALLOWED.has(name) ? [''] : []), ...options];
    if (!opts.includes(value)) opts.push(value);
    return (
      // displayEmpty + a shrunk label: an unset value («») shows its words instead of a blank box (HEU-18).
      <TextField select size="small" label={label} value={value} onChange={(e) => onChange!(path, e.target.value)}
        SelectProps={{ displayEmpty: true }} InputLabelProps={{ shrink: true }}
>
        {opts.map((o) => <MenuItem key={o || 'unset'} value={o}>{optLabel(name, o)}</MenuItem>)}
      </TextField>
    );
  }
  if (typeof value === 'string') {
    return <FormField label={label} value={value} onChange={(v) => onChange!(path, v)} />;
  }
  if (value && typeof value === 'object' && !Array.isArray(value)) {
    return (
      <Box sx={{ pl: 1.5, borderLeft: 2, borderColor: 'divider' }}>
        <Typography variant="overline" color="text.secondary">{label}</Typography>
        <Stack spacing={1.5} sx={{ mt: 0.5 }}>
          {Object.entries(value as JsonObject).map(([k, v]) => (
            <Field key={k} name={k} value={v} path={[...path, k]} onChange={onChange} enums={enums} limits={limits} errId={errId} />
          ))}
        </Stack>
      </Box>
    );
  }
  // null → read-only dash
  return <SettingRow label={label} value="—" />;
};

export const ShadowConfigEditor: React.FC<{
  domain: ConfigDomain; readOnly?: boolean;
  /** Show only these top-level sections (hardware-config → its `network` nominals in «Электросеть»). */
  only?: string[];
}> = ({ domain, readOnly, only }) => {
  const { desired, hasChanges, requiresReboot, load, editDoc } = useConfigShadow(domain);
  const session = useDeviceSession();
  const caps = useCapabilities();
  const enums = caps.stringEnums;
  const epoch = useFieldEpoch();
  const loadedRef = useRef(false);
  useEffect(() => () => clearFieldErrors(`${domain}.`), [domain]);

  useEffect(() => {
    if (!loadedRef.current) {
      loadedRef.current = true;
      load().catch(() => undefined);
    }
  }, [load]);

  const onChange = (path: string[], value: JsonValue) => editDoc(setAtPath(desired, path, value));

  // An explicit `only` wins over DEDICATED (system.ev is shown in its own card through it).
  const keys = Object.keys(desired).filter((k) => (only ? only.includes(k) : !DEDICATED.has(k)) && !HIDDEN.has(k) && !outOfScope(k) && !Array.isArray(desired[k])
    // Stored by the hub, read by nothing (root 18.10) — a switch that does nothing isn't shown.
    && !(domain === 'system' && SYSTEM_UNUSED_FIELDS.has(k)));
  if (keys.length === 0) {
    return (
      <Box sx={{ display: 'flex', justifyContent: 'center', py: 3 }}>
        <CircularProgress size={22} aria-label={t('common.loading')} />
      </Box>
    );
  }

  // Fields the hub reads only at start sit apart from the live ones, under their own heading
  // (operator 18.10: say which save needs a reboot) — a whole start-time section says it once.
  const atStart = readOnly ? [] : keys.filter((k) => session.fieldNeedsReboot(domain, k));
  const live = keys.filter((k) => !atStart.includes(k));
  // `only` = one nested section (system.ev in «Зарядные станции»): its fields straight in the
  // card, without a group heading repeating the card title.
  const flat = only?.length === 1 && desired[only[0]] && typeof desired[only[0]] === 'object' && !Array.isArray(desired[only[0]])
    ? only[0] : null;
  const field = (k: string) => (
    <Field key={k} name={k} value={desired[k]} path={[k]} onChange={readOnly ? undefined : onChange} enums={enums} limits={caps.limits} errId={domain} />
  );
  if (flat) {
    const sub = desired[flat] as JsonObject;
    const restart = !readOnly && session.fieldNeedsReboot(domain, flat);
    return (
      <Stack spacing={1.5} key={epoch}>
        {restart && (
          <Box>
            <Typography variant="overline" color="warning.main" sx={{ display: 'block', lineHeight: 1.6 }}>{t('configEditor.atStartTitle')}</Typography>
            <Typography variant="caption" color="text.secondary" sx={{ display: 'block' }}>{t('configEditor.atStartHint')}</Typography>
          </Box>
        )}
        {Object.keys(sub).map((k) => (
          <Field key={k} name={k} value={sub[k]} path={[flat, k]} onChange={readOnly ? undefined : onChange} enums={enums} limits={caps.limits} errId={domain} />
        ))}
      </Stack>
    );
  }

  return (
    <Stack spacing={1.5} key={epoch}>
      {live.map(field)}
      {atStart.length > 0 && (
        <Box sx={live.length ? { pt: 1, borderTop: 1, borderColor: 'divider' } : undefined}>
          <Typography variant="overline" color="warning.main" sx={{ display: 'block', lineHeight: 1.6 }}>
            {t('configEditor.atStartTitle')}
          </Typography>
          <Typography variant="caption" color="text.secondary" sx={{ display: 'block', mb: 1 }}>
            {t('configEditor.atStartHint')}
          </Typography>
          <Stack spacing={1.5}>{atStart.map(field)}</Stack>
        </Box>
      )}
      {readOnly && (
        <Typography variant="caption" color="text.secondary">
          {t('configEditor.readOnly')}
        </Typography>
      )}
      {hasChanges && (
        <Typography variant="caption" color={requiresReboot ? 'warning.main' : 'text.secondary'}>
          {requiresReboot ? t('configEditor.unsavedReboot') : t('configEditor.unsaved')}
        </Typography>
      )}
    </Stack>
  );
};
