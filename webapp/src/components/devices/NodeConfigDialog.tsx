/**
 * NodeConfigDialog — one ESP-NOW node's settings: per channel, on/off and the current clamp
 * (`enums.ct_model`), sent as `configure` (fw F3/F5, hub repo documents/api/ESPNOW_API.md). The
 * energy period is shown, not set: fixed at 15 min (root espnow-test-scenarios Q1). The node takes them on
 * its next contact and confirms; the card shows «settings not applied yet» until it does.
 * Opened only for a node whose hub reports its config (`period_s` / `config_state`).
 */
import React, { useEffect, useState } from 'react';
import {
  Alert, Button, Dialog, DialogActions, DialogContent, DialogTitle, MenuItem, Stack, Switch, TextField, Typography,
} from '@mui/material';
import { useCapabilities, EspnowNode, EspnowNodeConfig } from '../../core';
import { t } from '../../i18n';
import { ctOptions } from '../../entities/moduleLine';

/** The node's settings as a `configure` body. */
export function nodeConfigOf(n: EspnowNode): EspnowNodeConfig {
  return {
    channels: (n.channels ?? []).map((c) => {
      const held = n.config?.find((x) => x.ch === c.ch);
      return { ch: c.ch, enabled: (held?.enabled ?? c.enabled) !== false, ct_code: held?.ct_code ?? c.ct_code };
    }),
  };
}

export const NodeConfigDialog: React.FC<{
  node: EspnowNode | null;
  busy: boolean;
  onClose: () => void;
  onSave: (mac: string, config: EspnowNodeConfig) => void;
}> = ({ node, busy, onClose, onSave }) => {
  const caps = useCapabilities();
  const [draft, setDraft] = useState<EspnowNodeConfig | null>(null);
  useEffect(() => { setDraft(node ? nodeConfigOf(node) : null); }, [node]);
  if (!node || !draft) return null;

  const allOff = draft.channels.length > 0 && draft.channels.every((c) => !c.enabled);
  const before = nodeConfigOf(node);
  const changed = JSON.stringify(before) !== JSON.stringify(draft);
  const setCh = (ch: number, patch: Partial<EspnowNodeConfig['channels'][number]>) =>
    setDraft({ ...draft, channels: draft.channels.map((c) => (c.ch === ch ? { ...c, ...patch } : c)) });

  return (
    <Dialog open onClose={onClose} maxWidth="sm" fullWidth aria-labelledby="node-config-title">
      <DialogTitle id="node-config-title">{t('nodes.cfgTitle', { mac: node.mac })}</DialogTitle>
      <DialogContent>
        <Stack spacing={2} sx={{ pt: 1 }}>
          <Typography variant="body2" color="text.secondary">
            {t('nodes.cfgPeriodFixed', { n: Math.round((node.period_s ?? 900) / 60) })}
          </Typography>
          {draft.channels.map((c) => (
            <Stack key={c.ch} direction="row" spacing={1.5} alignItems="center" flexWrap="wrap" useFlexGap>
              <Typography variant="body2" sx={{ minWidth: 88 }}>{t('nodes.channelN', { ch: c.ch })}</Typography>
              <Switch checked={c.enabled} onChange={(e) => setCh(c.ch, { enabled: e.target.checked })}
                inputProps={{ 'aria-label': t('nodes.cfgChannelOn', { ch: c.ch }) }} />
              <TextField select size="small" label={t('nodes.cfgCt')} value={c.ct_code} disabled={!c.enabled} sx={{ minWidth: 220, flex: 1 }}
                onChange={(e) => setCh(c.ch, { ct_code: Number(e.target.value) })}>
                {ctOptions(caps.ctModels, c.ct_code).map(([code, name]) => <MenuItem key={code} value={code}>{name}</MenuItem>)}
              </TextField>
            </Stack>
          ))}
          <Typography variant="body2" color="text.secondary">{t('nodes.cfgCtHint')}</Typography>
          {allOff && <Alert severity="warning">{t('nodes.cfgAllOff')}</Alert>}
          {!node.online && <Alert severity="info">{t('nodes.cfgOffline')}</Alert>}
        </Stack>
      </DialogContent>
      <DialogActions>
        <Button onClick={onClose}>{t('common.cancel')}</Button>
        <Button variant="contained" disabled={busy || !changed || allOff} onClick={() => onSave(node.mac, draft)}>{t('nodes.cfgSend')}</Button>
      </DialogActions>
    </Dialog>
  );
};
