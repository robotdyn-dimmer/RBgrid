/**
 * GroupsPanel — consumer-group names on the device (root e139ab5e: system-config `groups[]`).
 * Add / rename / remove; Save sends the WHOLE list (the hub replaces it), live, persisted.
 * Membership is set per point (UnitParamsPanel «Группа»); the count of points per group comes
 * from units-config. Removing a group that still has points leaves them with that group_id —
 * the UI then names it by its fallback (trunk → members → «Группа N»), so the panel warns.
 * Shown only when the hub reports `groups` (older firmware has no names).
 */
import React, { useEffect, useMemo, useState } from 'react';
import { Alert, Box, Button, IconButton, Stack, TextField, Typography } from '@mui/material';
import DeleteOutlineIcon from '@mui/icons-material/DeleteOutline';
import { PanelCard } from '@rbgrid/design-system';
import { useCapabilities, useConfigShadow } from '../../core';
import type { JsonObject } from '../../core';
import { useDeviceGroups, DeviceGroup, MAX_GROUPS } from '../../hooks/useDeviceGroups';
import { utf8Len } from '../common/utf8';
import { t } from '../../i18n';
import { useUnsavedChanges } from '../../app/unsavedGuard';
import { savedText } from '../../services/savedText';

export const GroupsPanel: React.FC = () => {
  const caps = useCapabilities();
  const { groups, supported, save } = useDeviceGroups();
  const units = useConfigShadow('units');
  const [draft, setDraft] = useState<DeviceGroup[]>(groups);
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState<{ sev: 'success' | 'error'; text: string } | null>(null);
  const key = JSON.stringify(groups);
  useEffect(() => { setDraft(groups); }, [key]); // eslint-disable-line react-hooks/exhaustive-deps

  const members = useMemo(() => {
    const m = new Map<number, number>();
    const list = Array.isArray(units.reported.units) ? (units.reported.units as JsonObject[]) : [];
    list.forEach((u) => { if (typeof u.group_id === 'number' && u.group_id > 0) m.set(u.group_id, (m.get(u.group_id) ?? 0) + 1); });
    return m;
  }, [units.reported]);

  useUnsavedChanges('groups', supported && JSON.stringify(draft.map((g) => ({ ...g, name: g.name.trim() }))) !== key);

  if (!supported) return null;
  const maxBytes = caps.limits.nameBytes;
  const tooLong = (g: DeviceGroup) => utf8Len(g.name.trim()) > maxBytes;
  const invalid = draft.some((g) => !g.name.trim() || tooLong(g));
  const dirty = JSON.stringify(draft.map((g) => ({ ...g, name: g.name.trim() }))) !== key;
  const orphaned = groups.filter((g) => !draft.some((d) => d.id === g.id) && (members.get(g.id) ?? 0) > 0);
  const nextId = () => { for (let i = 1; i <= 255; i++) if (!draft.some((g) => g.id === i)) return i; return 0; };

  const onSave = async () => {
    setBusy(true); setMsg(null);
    try {
      await save(draft.map((g) => ({ id: g.id, name: g.name.trim() })));
      setMsg({ sev: 'success', text: savedText('groups.saved') });
    } catch (e) {
      const err = e as { status?: number };
      setMsg({ sev: 'error', text: err.status === 401 || err.status === 403 ? t('common.adminTokenRequired') : t('common.deviceRejected') });
    } finally { setBusy(false); }
  };

  return (
    <Box id="groups" sx={{ scrollMarginTop: 16 }}>
      <PanelCard title={t('groups.title')}>
        <Stack spacing={1.5}>
          <Typography variant="body2" color="text.secondary">{t('groups.intro')}</Typography>
          {draft.length === 0 && <Typography variant="body2" color="text.secondary">{t('groups.empty')}</Typography>}
          {draft.map((g, i) => (
            <Stack key={g.id} direction="row" spacing={1} alignItems="flex-start">
              <TextField
                size="small" fullWidth label={t('groups.name', { id: g.id })} value={g.name}
                onChange={(e) => setDraft(draft.map((x, j) => (j === i ? { ...x, name: e.target.value } : x)))}
                error={!g.name.trim() || tooLong(g)}
                helperText={`${t('groups.points', { count: members.get(g.id) ?? 0 })} · ${t('unitParams.bytesUsed', { used: utf8Len(g.name.trim()), max: maxBytes })}`}
              />
              <IconButton aria-label={t('groups.remove', { name: g.name || g.id })} onClick={() => setDraft(draft.filter((_, j) => j !== i))} sx={{ mt: 0.25 }}>
                <DeleteOutlineIcon fontSize="small" />
              </IconButton>
            </Stack>
          ))}
          {orphaned.length > 0 && <Alert severity="warning">{t('groups.orphaned', { names: orphaned.map((g) => g.name).join(', ') })}</Alert>}
          {msg && <Alert severity={msg.sev}>{msg.text}</Alert>}
          <Stack direction="row" spacing={1} flexWrap="wrap" useFlexGap>
            <Button disabled={draft.length >= MAX_GROUPS || busy} onClick={() => setDraft([...draft, { id: nextId(), name: '' }])}>
              {t('groups.add')}
            </Button>
            <Box sx={{ flex: 1 }} />
            <Stack direction="row" spacing={1}>
              <Button variant="contained" disabled={!dirty || invalid || busy} onClick={onSave}>{busy ? t('common.saving') : t('common.save')}</Button>
              <Button disabled={!dirty || busy} onClick={() => { setDraft(groups); setMsg(null); }}>{t('common.cancel')}</Button>
            </Stack>
          </Stack>
        </Stack>
      </PanelCard>
    </Box>
  );
};
