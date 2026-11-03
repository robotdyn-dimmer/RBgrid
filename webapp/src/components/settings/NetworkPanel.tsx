/**
 * NetworkPanel — WiFi status + scan + connect (save credentials).
 * Renders through the @rbgrid/design-system settings surface (PanelCard / NetworkStatus / FormField);
 * data + flow logic stay here, presentation is single-brand.
 */
import React, { useEffect, useState } from 'react';
import {
  Button, Stack, List, ListItemButton, ListItemText, Chip, CircularProgress, Box,
} from '@mui/material';
import WifiIcon from '@mui/icons-material/Wifi';
import { PanelCard, NetworkStatus, FormField } from '@rbgrid/design-system';
import { getApiClient } from '../../services/api';
import { WifiStatus } from '../../types/network';
import { WiFiScanNetwork } from '../../types/api';
import { t } from '../../i18n';
import { useConfirm } from '../common/ConfirmDialog';

/** WiFi mode in words (UX audit W0-10: «Mode STA» meant nothing to a home user). */
const modeWord = (mode: string | undefined): string => {
  const m = String(mode ?? '').toUpperCase().replace(/[^A-Z]/g, '');
  if (m === 'STA') return t('network.modeSta');
  if (m === 'AP') return t('network.modeAp');
  if (m === 'APSTA' || m === 'STAAP') return t('network.modeApSta');
  return mode || '—';
};
/** Signal in words: ≥ −60 dBm good, ≥ −72 fair, below weak (typical Wi-Fi thresholds). */
const rssiWordKey = (dbm: number): string => (dbm >= -60 ? 'network.rssiGood' : dbm >= -72 ? 'network.rssiFair' : 'network.rssiWeak');

export const NetworkPanel: React.FC = () => {
  const [status, setStatus] = useState<WifiStatus | null>(null);
  const [networks, setNetworks] = useState<WiFiScanNetwork[]>([]);
  const [scanning, setScanning] = useState(false);
  const confirm = useConfirm();
  const [ssid, setSsid] = useState('');
  const [password, setPassword] = useState('');
  const [msg, setMsg] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);

  useEffect(() => {
    let on = true;
    getApiClient().getWifiStatus().then((s) => { if (on) setStatus(s); }).catch(() => {});
    return () => { on = false; };
  }, []);

  const scan = async () => {
    setScanning(true);
    setMsg(null);
    try {
      setNetworks(await getApiClient().scanWiFiNetworks());
    } catch {
      setMsg(t('network.scanFailed'));
    } finally {
      setScanning(false);
    }
  };

  const save = async () => {
    if (!ssid) return;
    if (!(await confirm({ title: t('network.saveConfirm', { ssid }) }))) return;
    setSaving(true);
    setMsg(null);
    try {
      await getApiClient().saveWifi({ ssid, password });
      setMsg(t('network.saved'));
    } catch {
      setMsg(t('network.saveFailed'));
    } finally {
      setSaving(false);
    }
  };

  return (
    <Stack spacing={2}>
      <PanelCard title={t('network.status')}>
        {!status ? (
          <Box sx={{ color: 'text.secondary' }}>—</Box>
        ) : (
          <NetworkStatus
            mode={modeWord(status.mode)}
            ssid={status.ssid || status.ap_ssid || ''}
            ip={status.sta_ip || status.ap_ip || ''}
            rssi={status.rssi}
            rssiText={status.rssi != null ? t('network.rssi', { dbm: status.rssi, word: t(rssiWordKey(status.rssi)) }) : undefined}
            labels={{ mode: t('network.lblMode'), ssid: t('network.lblSsid'), ip: t('network.lblIp'), rssi: t('network.lblRssi') }}
          />
        )}
      </PanelCard>

      <PanelCard
        title={t('network.connection')}
        actionLabel={scanning ? t('network.scanning') : t('network.scan')}
        actionIcon={scanning ? <CircularProgress size={16} aria-label={t('common.loading')} /> : <WifiIcon />}
        actionDisabled={scanning}
        onAction={scan}
        alert={msg ?? undefined}
        alertSeverity="info"
      >
        {networks.length > 0 && (
          <List dense sx={{ maxHeight: 200, overflow: 'auto', mb: 1 }}>
            {networks.map((n, i) => (
              <ListItemButton key={`${n.bssid}-${i}`} onClick={() => setSsid(n.ssid)}>
                <ListItemText primary={n.ssid || t('network.hidden')} secondary={t('network.netInfo', { auth: n.auth_mode, channel: n.channel })} />
                <Chip size="small" label={`${n.rssi} dBm`} variant="outlined" />
              </ListItemButton>
            ))}
          </List>
        )}
        <Stack spacing={2}>
          <FormField label="SSID" value={ssid} onChange={setSsid} fullWidth />
          <FormField label={t('network.password')} type="password" value={password} onChange={setPassword} fullWidth />
          <Box>
            <Button variant="contained" onClick={save} disabled={saving || !ssid}>
              {saving ? t('common.saving') : t('network.saveReboot')}
            </Button>
          </Box>
        </Stack>
      </PanelCard>
    </Stack>
  );
};
