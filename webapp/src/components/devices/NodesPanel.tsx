/**
 * NodesPanel — the ESP-NOW remote nodes (hub + wireless metering nodes), contract fw 1.1.7
 * (hub repo documents/api/ESPNOW_API.md, root espnow-nodes-ui).
 *
 * - Availability: `keys:false` → «not available in this firmware». A hub before 1.1.7 sends no
 *   `keys`: shown only in a preview build (release images had no keys).
 * - The radio: on / off (`hub_on` / `hub_off`, persisted); `auto` starts it once a node is registered.
 *   The radio channel follows the Wi-Fi router, so it's shown, not set.
 * - Pairing: a window of 1–10 min with a countdown from `pairing_left_s`, cancel, the nodes found in
 *   it marked «new». Since F2 (77964b4) a paired node's points exist at once, with history: no restart;
 *   `paired_count` is just how many joined in this window.
 * - Limits: N of `max_nodes`; points with history share the hub's 12 slots — said before pairing.
 * - Node cards: online / last seen, link loss and sync, channels → their points, disable / enable,
 *   remove (its points and their history go with it). Fields of F3/F5 (fw, rssi, held frames,
 *   period, config state) show when a hub sends them; a node whose hub reports its config
 *   (`period_s` / `config_state`) gets «Settings» → `configure` (NodeConfigDialog).
 */
import React, { useEffect, useRef, useState } from 'react';
import { Link as RouterLink } from 'react-router-dom';
import {
  Box, Typography, Stack, Button, Alert, CircularProgress, Chip, Switch, FormControlLabel, MenuItem, TextField, Link,
} from '@mui/material';
import WifiTetheringIcon from '@mui/icons-material/WifiTethering';
import { Card, StatusChip, InfoField } from '@rbgrid/design-system';
import { useEspnow, useDeviceSession, EspnowAction, EspnowActionParams, EspnowActionResult, EspnowNode, EspnowNodeConfig } from '../../core';
import { useUnitsConfig } from '../../hooks/useUnitsConfig';
import { hexKey } from '../../entities/siteModel';
import { espnowAvailable } from '../../core/espnow/available';
import { t } from '../../i18n';
import { RebootNowButton } from '../common/RebootNowButton';
import { useConfirm } from '../common/ConfirmDialog';
import { HISTORY_SLOTS } from '../common/NoHistoryNote';
import { PanelBoundary } from '../common/PanelBoundary';
import { NodeConfigDialog } from './NodeConfigDialog';

const PAIR_WINDOWS_S = [60, 120, 300, 600];
const POLL_MS = 5000;
const POLL_PAIRING_MS = 2000;

type Msg = { sev: 'success' | 'info' | 'warning' | 'error'; text: string; reboot?: boolean };

/** «now» / «12 s ago» / «5 min ago» / «3 h ago». */
export function lastSeenText(s: number | undefined): string {
  if (s == null || s < 0) return t('nodes.notHeard');
  if (s <= 2) return t('nodes.now');
  if (s < 120) return t('nodes.agoS', { n: s });
  if (s < 7200) return t('nodes.agoMin', { n: Math.round(s / 60) });
  return t('nodes.agoH', { n: Math.round(s / 3600) });
}

/** A refusal or a lost request → the reader's words. */
export function nodesFailText(res: EspnowActionResult | null, status?: number): string {
  if (status === 401 || status === 403) return t('common.adminTokenRequired');
  switch (res?.error) {
    case 'ESPNOW_DISABLED':
    case 'ESPNOW_OFF': return t('nodes.errOff');
    case 'ESPNOW_START_FAILED': return t('nodes.errStart');
    case 'REGISTRY': return t('nodes.errRegistry');
    case 'BAD_REQUEST': return t('nodes.errBadRequest');
    case 'NOT_FOUND': return t('nodes.errNotFound');
    case 'PERIOD_UNSUPPORTED': return t('nodes.errPeriod');
  }
  return res?.message ? t('nodes.errHub', { message: res.message }) : t('nodes.actionFailedHint');
}

export const NodesPanel: React.FC = () => (
  <PanelBoundary name={t('nodes.title')}><NodesPanelBody /></PanelBoundary>
);

const NodesPanelBody: React.FC = () => {
  const { data, loading, reloadNow } = useEspnow();
  const session = useDeviceSession();
  const units = useUnitsConfig();
  const confirm = useConfirm();
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState<Msg | null>(null);
  const [pairS, setPairS] = useState(120);
  // Nodes the hub already had when this pairing started — the rest are «new».
  const before = useRef<Set<string> | null>(null);
  // The countdown runs locally between polls: the end time from the hub's last `pairing_left_s`.
  const [pairEnd, setPairEnd] = useState<number | null>(null);
  const [, tick] = useState(0);
  const [editing, setEditing] = useState<string | null>(null);

  const pairingOpen = !!data?.pairing_open;
  useEffect(() => {
    if (!data) return;
    setPairEnd(data.pairing_open && data.pairing_left_s != null ? Date.now() + data.pairing_left_s * 1000 : null);
    if (!data.pairing_open && data.paired_count === 0) before.current = null;
  }, [data]);

  // A node paired or removed adds or drops points: re-read units-config, so names, links and the
  // history-slot count follow (root espnow-scenarios-results-1: «History: 4 of 12» after a delete).
  const topology = data?.nodes?.map((n) => `${n.mac}:${(n.channels ?? []).map((c) => c.unit_id).join(',')}`).join(';');
  // loadDomain joins a read already in flight, which may predate the change, so wait for it and read
  // again; one more read a moment later catches history flags the hub sets just after the node shows
  // (results-2 B5: «2 of 12» after a fast delete → pair, hub had 4).
  const seenTopology = useRef<string | undefined>(undefined);
  useEffect(() => {
    if (topology === undefined) return;
    const changed = seenTopology.current !== undefined && seenTopology.current !== topology;
    seenTopology.current = topology;
    if (!changed) return;
    const reread = () => session.loadDomain('units').catch(() => {});
    reread().then(reread);
    const late = setTimeout(reread, 3000);
    return () => clearTimeout(late);
  }, [topology, session]);

  // Quiet refresh: faster while the window is open, so a new node and the countdown show promptly.
  useEffect(() => {
    if (!data || data.keys === false) return;
    const id = setInterval(() => { void reloadNow(true); }, pairingOpen ? POLL_PAIRING_MS : POLL_MS);
    return () => clearInterval(id);
  }, [data, pairingOpen, reloadNow]);
  useEffect(() => {
    if (pairEnd == null) return;
    const id = setInterval(() => tick((n) => n + 1), 1000);
    return () => clearInterval(id);
  }, [pairEnd]);

  if (!data) {
    return loading ? (
      <Box sx={{ mb: 3 }}><Card><Box sx={{ display: 'flex', justifyContent: 'center', py: 2 }}><CircularProgress size={22} aria-label={t('common.loading')} /></Box></Card></Box>
    ) : null;
  }
  // A hub before 1.1.7 says nothing about keys: its release images had none.
  if (data.keys === undefined && !espnowAvailable(undefined)) return null;
  if (data.keys === false) {
    return (
      <Box sx={{ mb: 3 }}>
        <Card>
          <Typography variant="h6" fontWeight={700} mb={1}>{t('nodes.title')}</Typography>
          <Typography variant="body2" color="text.secondary">{t('nodes.notAvailable')}</Typography>
        </Card>
      </Box>
    );
  }

  const act = async (action: EspnowAction, params: EspnowActionParams = {}): Promise<EspnowActionResult | null> => {
    setBusy(true);
    setMsg(null);
    const job = session.jobs.start('espnow', { action, ...(params.mac ? { mac: params.mac } : {}) });
    try {
      const res = await session.transport.espnowAction(action, params);
      if (!res.ok) {
        session.jobs.fail(job.id, res.error ?? 'refused');
        setMsg({ sev: 'error', text: nodesFailText(res) });
        return res;
      }
      if (res.reboot_to_register) {
        session.jobs.awaitReboot(job.id);
        setMsg({ sev: 'info', text: t('nodes.acceptedReboot'), reboot: true });
      } else {
        session.jobs.resolve(job.id);
      }
      await reloadNow(true);
      return res;
    } catch (e) {
      session.jobs.fail(job.id, 'request failed');
      setMsg({ sev: 'error', text: nodesFailText(null, (e as { response?: { status?: number } })?.response?.status) });
      return null;
    } finally {
      setBusy(false);
    }
  };

  const nodes = data.nodes ?? [];
  const maxNodes = data.max_nodes ?? 6;
  const perNode = data.max_channels_per_node ?? 4;
  const full = nodes.length >= maxNodes;
  const radioOn = data.enabled ? data.enabled !== 'off' : data.running;
  const left = pairEnd != null ? Math.max(0, Math.ceil((pairEnd - Date.now()) / 1000)) : 0;
  const isNew = (mac: string) => before.current != null && !before.current.has(mac);
  const histUsed = units.historyCount;

  const toggleRadio = async (on: boolean) => {
    if (!on && nodes.length > 0 && !(await confirm({ title: t('nodes.offTitle'), body: t('nodes.offBody'), confirmLabel: t('nodes.offConfirm') }))) return;
    await act(on ? 'hub_on' : 'hub_off');
  };
  const startPairing = async () => {
    before.current = new Set(nodes.map((n) => n.mac));
    const res = await act('pair', { seconds: pairS });
    if (!res?.ok) before.current = null;
  };
  // Turning a node off discards its data: the hub acknowledges its periods and drops them, and
  // `enable` brings no backfill (root espnow-test-scenarios Q3).
  const toggleNode = async (n: EspnowNode) => {
    if (!n.disabled && !(await confirm({ title: t('nodes.disableTitle', { mac: n.mac }), body: t('nodes.disableBody'), confirmLabel: t('nodes.disable') }))) return;
    await act(n.disabled ? 'enable' : 'disable', { mac: n.mac });
  };
  const remove = async (n: EspnowNode) => {
    if (!(await confirm({ title: t('nodes.removeTitle', { mac: n.mac }), body: t('nodes.removeBody', { n: n.channels?.length ?? 0 }), confirmLabel: t('common.delete'), danger: true }))) return;
    const res = await act('remove', { mac: n.mac });
    if (res?.ok) setMsg({ sev: 'success', text: t('nodes.removed', { n: res.removed_units ?? n.channels?.length ?? 0 }) });
  };

  const saveConfig = async (mac: string, config: EspnowNodeConfig) => {
    const res = await act('configure', { mac, config });
    if (res?.ok) {
      setEditing(null);
      setMsg({ sev: 'info', text: t('nodes.cfgSent') });
    }
  };
  const editNode = nodes.find((n) => n.mac === editing) ?? null;

  const radioState = data.running ? t('nodes.radioRunning', { channel: data.channel })
    : data.enabled === 'auto' ? t('nodes.radioAuto')
      : t('nodes.radioOff');

  return (
    <Box sx={{ mb: 3 }}>
      <Card>
        <Stack direction="row" alignItems="center" spacing={1.5} mb={0.5} flexWrap="wrap" useFlexGap>
          <Typography variant="h6" fontWeight={700}>{t('nodes.title')}</Typography>
          <StatusChip status={data.running ? 'live' : 'dormant'} label={data.running ? t('nodes.running') : t('nodes.stopped')} />
          <Typography variant="body2" color="text.secondary" sx={{ ml: 'auto' }}>{t('nodes.count', { n: nodes.length, max: maxNodes })}</Typography>
        </Stack>
        <Stack direction="row" alignItems="center" spacing={1} flexWrap="wrap" useFlexGap mb={1.5}>
          <FormControlLabel
            control={<Switch checked={radioOn} disabled={busy} onChange={(e) => { void toggleRadio(e.target.checked); }} />}
            label={t('nodes.radio')}
          />
          <Typography variant="body2" color="text.secondary">{radioState}</Typography>
        </Stack>

        {msg && <Alert severity={msg.sev} sx={{ mb: 2 }} onClose={() => setMsg(null)} action={msg.reboot ? <RebootNowButton /> : undefined}>{msg.text}</Alert>}
        {data.paired_count > 0 && <Alert severity="success" sx={{ mb: 2 }}>{t('nodes.paired', { n: data.paired_count })}</Alert>}

        {/* Pairing */}
        <Box sx={{ border: 1, borderColor: 'divider', borderRadius: 2, p: 1.5, mb: 2 }}>
          {pairingOpen ? (
            <Stack direction="row" alignItems="center" spacing={1.5} flexWrap="wrap" useFlexGap>
              <CircularProgress size={18} />
              <Typography variant="body2" sx={{ fontVariantNumeric: 'tabular-nums' }}>{t('nodes.pairingLeft', { time: `${Math.floor(left / 60)}:${String(left % 60).padStart(2, '0')}` })}</Typography>
              <Button size="small" disabled={busy} onClick={() => { void act('pair_cancel'); }} sx={{ ml: 'auto' }}>{t('nodes.pairCancel')}</Button>
            </Stack>
          ) : (
            <Stack direction="row" alignItems="center" spacing={1.5} flexWrap="wrap" useFlexGap>
              <TextField select size="small" label={t('nodes.pairWindow')} value={pairS} onChange={(e) => setPairS(Number(e.target.value))} sx={{ minWidth: 140 }}>
                {PAIR_WINDOWS_S.map((s) => <MenuItem key={s} value={s}>{t('nodes.minutes', { n: s / 60 })}</MenuItem>)}
              </TextField>
              <Button variant="outlined" startIcon={<WifiTetheringIcon />} disabled={busy || full || !radioOn} onClick={() => { void startPairing(); }}>
                {t('nodes.pair')}
              </Button>
            </Stack>
          )}
          <Typography variant="body2" color="text.secondary" mt={1}>
            {full ? t('nodes.full', { max: maxNodes })
              : !radioOn ? t('nodes.pairNeedsRadio')
                : pairingOpen ? t('nodes.pairingHow') : t('nodes.pairIntro', { n: perNode })}
          </Typography>
          {!full && histUsed != null && (
            <Typography variant="body2" color="text.secondary" mt={0.5}>
              {histUsed >= HISTORY_SLOTS ? t('nodes.historyFull', { n: HISTORY_SLOTS }) : t('nodes.historyLeft', { used: histUsed, n: HISTORY_SLOTS })}
            </Typography>
          )}
        </Box>

        {nodes.length === 0 && <Typography color="text.secondary" variant="body2">{t('nodes.empty')}</Typography>}

        <Stack spacing={1.5}>
          {nodes.map((n) => (
            <Box key={n.mac} sx={{ border: 1, borderColor: 'divider', borderRadius: 2, p: 1.5, opacity: n.disabled ? 0.7 : 1 }}>
              <Stack direction="row" alignItems="center" spacing={1} mb={1} flexWrap="wrap" useFlexGap>
                <Typography variant="subtitle2" fontWeight={700} sx={{ fontVariantNumeric: 'tabular-nums' }}>{n.mac}</Typography>
                <StatusChip status={n.online ? 'live' : 'offline'} label={n.online ? t('nodes.online') : t('nodes.offline')} />
                {isNew(n.mac) && <Chip size="small" color="success" label={t('nodes.new')} />}
                {n.disabled && <Chip size="small" variant="outlined" label={t('nodes.disabled')} />}
                {n.config_state === 'pending' && <Chip size="small" variant="outlined" label={t('nodes.configPending')} />}
                {n.config_state === 'failed' && <Chip size="small" color="error" variant="outlined" label={t('nodes.configFailed')} />}
                <Stack direction="row" spacing={1} sx={{ ml: 'auto' }}>
                  {(n.period_s != null || n.config_state != null) && (
                    <Button size="small" disabled={busy} onClick={() => setEditing(n.mac)}>{t('nodes.settings')}</Button>
                  )}
                  <Button size="small" disabled={busy} onClick={() => { void toggleNode(n); }}>
                    {n.disabled ? t('nodes.enable') : t('nodes.disable')}
                  </Button>
                  <Button size="small" color="error" disabled={busy} onClick={() => { void remove(n); }}>{t('common.delete')}</Button>
                </Stack>
              </Stack>
              {n.disabled && <Typography variant="body2" color="text.secondary" mb={1}>{t('nodes.disabledHint')}</Typography>}
              <Stack direction="row" spacing={3} flexWrap="wrap" useFlexGap sx={{ mb: 1 }}>
                <InfoField label={t('nodes.lastSeen')} value={lastSeenText(n.last_seen_s)} />
                {n.sync && <InfoField label={t('nodes.loss')} value={typeof n.sync.loss_pct === 'number' ? `${Math.round(n.sync.loss_pct * 10) / 10} %` : t('nodes.lossPending')} />}
                {n.sync && <InfoField label={t('nodes.sync')} value={t('nodes.syncValue', { us: Math.round(n.sync.resid_sd_us), ppm: Math.round(n.sync.skew_ppm * 10) / 10 })} />}
                {n.rssi_dbm != null && <InfoField label={t('nodes.signal')} value={`${n.rssi_dbm} dBm`} />}
                {n.fw && <InfoField label={t('nodes.fw')} value={n.fw} />}
                {n.period_s != null && <InfoField label={t('nodes.period')} value={t('nodes.minutes', { n: Math.round(n.period_s / 60) })} />}
                {n.held_frames != null && n.held_frames > 0 && <InfoField label={t('nodes.held')} value={String(n.held_frames)} />}
              </Stack>
              {(n.channels ?? []).map((c) => {
                const key = hexKey(c.unit_id);
                const name = units.forKey?.(c.unit_id)?.name;
                return (
                  <Stack key={c.ch} direction="row" spacing={1} alignItems="baseline" sx={{ py: 0.25 }}>
                    <Typography variant="body2" color="text.secondary" sx={{ minWidth: 88 }}>{t('nodes.channelN', { ch: c.ch })}</Typography>
                    {key
                      ? <Link component={RouterLink} to={`/point/${key}`} variant="body2">{name ?? t('nodes.pointKey', { key })}</Link>
                      : <Typography variant="body2">{c.unit_id}</Typography>}
                    <Typography variant="body2" color="text.secondary" sx={{ ml: 'auto' }}>{n.config?.find((x) => x.ch === c.ch)?.enabled === false ? t('nodes.channelOff')
                      : c.present && n.online && !n.disabled ? t('nodes.receiving') : t('nodes.noData')}</Typography>
                  </Stack>
                );
              })}
            </Box>
          ))}
        </Stack>
      </Card>
      <NodeConfigDialog node={editNode} busy={busy} onClose={() => setEditing(null)} onSave={(mac, cfg) => { void saveConfig(mac, cfg); }} />
    </Box>
  );
};
