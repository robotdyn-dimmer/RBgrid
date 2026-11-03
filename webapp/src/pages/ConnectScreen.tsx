/**
 * ConnectScreen — the app's first step: point at a device (enter its IP/URL) or explore in
 * demo mode. On connect we do a light reachability check (any HTTP response = reachable;
 * the device sends CORS:* so a cross-origin probe works from the dev server).
 */
import React, { useState } from 'react';
import axios, { AxiosError } from 'axios';
import {
  Box, Typography, Button, Stack, Divider, Alert, CircularProgress,
  ToggleButton, ToggleButtonGroup,
} from '@mui/material';
import BoltIcon from '@mui/icons-material/Bolt';
import { Card, FormField } from '@rbgrid/design-system';
import { connectDevice, connectBroker, connectDemo, normalizeBaseUrl, lastDevice, EMBEDDED } from '../services/connection';
import { RELEASE_HIDDEN } from '../constants/release';

/** The broker way is offered only where it can work: not in the hub kit, not while it is partial. */
const BROKER = !EMBEDDED && !RELEASE_HIDDEN.brokerTransport;
import { t } from '../i18n';

export const ConnectScreen: React.FC = () => {
  const [mode, setMode] = useState<'direct' | 'broker'>('direct');
  // Empty unless this browser used a device before — a prefilled stranger's IP got «connected»
  // as is (UX audit HOME-07).
  const [host, setHost] = useState(lastDevice);
  const [brokerUrl, setBrokerUrl] = useState('');
  const [mac, setMac] = useState('');
  const [testing, setTesting] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const connect = async () => {
    if (mode === 'broker') {
      if (!brokerUrl.trim() || !mac.trim()) return;
      connectBroker(brokerUrl, mac); // mqtt connects async; the gate re-renders into the app
      return;
    }
    const url = normalizeBaseUrl(host);
    if (!url) return;
    setTesting(true);
    setError(null);
    try {
      await axios.get(`${url}/wifi/status`, { timeout: 4000 });
      connectDevice(host); // reachable → commit (the gate re-renders into the app)
    } catch (e) {
      const ax = e as AxiosError;
      if (ax.response) {
        connectDevice(host); // got an HTTP response → device is reachable
      } else {
        // From an https page (webapp.rbgrid.io) Chrome / Edge ask once to allow devices on the local
        // network; a denied prompt looks like an unreachable hub (measured 04.11).
        const https = typeof window !== 'undefined' && window.location.protocol === 'https:';
        setError(https ? `${t('connect.unreachable')} ${t('connect.localNetworkHint')}` : t('connect.unreachable'));
      }
    } finally {
      setTesting(false);
    }
  };

  const canConnect = mode === 'broker' ? !!brokerUrl.trim() && !!mac.trim() : !!host.trim();

  return (
    <Box sx={{ minHeight: '100vh', display: 'flex', alignItems: 'center', justifyContent: 'center', bgcolor: 'background.default', p: 2 }}>
      <Box sx={{ width: '100%', maxWidth: 420 }}>
        <Card>
          <Stack direction="row" alignItems="center" spacing={1.5} mb={1}>
            <BoltIcon color="primary" sx={{ fontSize: 32 }} />
            <Typography variant="h5" fontWeight={800}>RBGrid</Typography>
          </Stack>
          <Typography variant="body2" color="text.secondary" mb={2}>
            {t(BROKER ? 'connect.intro' : 'connect.introHub')}
          </Typography>

          {/* The hub-installed kit leaves the MQTT transport out (build-embedded skips its chunk), so the
              broker way isn't offered there — it could only fail. */}
          {BROKER && (
            <ToggleButtonGroup
              exclusive size="small" color="primary" value={mode} fullWidth sx={{ mb: 2 }}
              onChange={(_, v) => v && setMode(v)}
            >
              <ToggleButton value="direct">{t('connect.modeDirect')}</ToggleButton>
              <ToggleButton value="broker">{t('connect.modeBroker')}</ToggleButton>
            </ToggleButtonGroup>
          )}

          {error && <Alert severity="error" sx={{ mb: 2 }}>{error}</Alert>}

          <Stack spacing={2}>
            {mode === 'direct' ? (
              <>
              <FormField
                label={t('connect.hostLabel')}
                value={host}
                onChange={setHost}
                onKeyDown={(e) => { if (e.key === 'Enter') connect(); }}
                autoFocus
                placeholder={t('connect.hostPlaceholder')}
                disabled={testing}
              />
              <Typography variant="caption" color="text.secondary" sx={{ mt: -1 }}>{t('connect.hostHelp')}</Typography>
              </>
            ) : (
              <>
                <FormField label={t('connect.brokerLabel')} value={brokerUrl} onChange={setBrokerUrl} placeholder={t('connect.brokerPlaceholder')} />
                <FormField label={t('connect.macLabel')} value={mac} onChange={setMac} placeholder="aabbccddeeff" />
                <Alert severity="info">{t('connect.brokerInfo')}</Alert>
              </>
            )}
            <Button
              variant="contained" size="large" onClick={connect}
              disabled={testing || !canConnect}
              startIcon={testing ? <CircularProgress size={18} aria-label={t('common.loading')} /> : undefined}
            >
              {testing ? t('connect.connecting') : t('connect.connect')}
            </Button>
            {error && (
              <Button variant="text" size="small" onClick={() => connectDevice(host)} disabled={testing}>
                {t('connect.connectAnyway')}
              </Button>
            )}
          </Stack>

          <Divider sx={{ my: 3 }}>{t('connect.or')}</Divider>

          <Button fullWidth variant="outlined" onClick={connectDemo}>
            {t('connect.openDemo')}
          </Button>
        </Card>
      </Box>
    </Box>
  );
};
