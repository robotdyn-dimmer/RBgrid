import React from 'react';
import { Stack } from '@mui/material';
import { InfoField } from './InfoField';

export interface NetworkStatusLabels {
  mode: string;
  ssid: string;
  ip: string;
  rssi: string;
}

export interface NetworkStatusProps {
  /** WiFi mode (e.g. "STA", "AP") — or the app's words for it. */
  mode: string;
  /** Connected / AP SSID. */
  ssid: string;
  /** Station or AP IP address. */
  ip: string;
  /** Signal strength in dBm (null = unknown). */
  rssi?: number | null;
  /** Signal shown instead of «-62 dBm» (e.g. the app's «−62 дБм · хороший»). */
  rssiText?: string;
  /** Field labels in the app's language (English by default). */
  labels?: Partial<NetworkStatusLabels>;
}

const DEFAULT_LABELS: NetworkStatusLabels = { mode: 'Mode', ssid: 'SSID', ip: 'IP', rssi: 'RSSI' };

/**
 * A WiFi status glance — mode / SSID / IP / RSSI as a row of InfoFields.
 * @category Settings
 */
export const NetworkStatus: React.FC<NetworkStatusProps> = ({ mode, ssid, ip, rssi, rssiText, labels }) => {
  const l = { ...DEFAULT_LABELS, ...labels };
  return (
    <Stack direction="row" spacing={3} flexWrap="wrap" useFlexGap>
      <InfoField label={l.mode} value={mode} />
      <InfoField label={l.ssid} value={ssid || '—'} />
      <InfoField label={l.ip} value={ip || '—'} />
      <InfoField label={l.rssi} value={rssiText ?? (rssi != null ? `${rssi} dBm` : '—')} />
    </Stack>
  );
};
