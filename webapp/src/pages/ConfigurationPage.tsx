/**
 * ConfigurationPage — settings as Device-Shadow reconciliation. Edit config domains freely
 * (each a generic ShadowConfigEditor), see the combined diff in the sticky reconcile bar,
 * and Apply once (desired → reported convergence; reboot-domains surface a warning first).
 * Capability-gated: a domain panel renders only when the device advertises it.
 * The single settings hub — reconciliation editors + the action panels (network/admin/
 * profile/security).
 */
import React, { useEffect } from 'react';
import { useLocation } from 'react-router-dom';
import { Box, Typography, Button, Divider } from '@mui/material';
import { useNavigate } from 'react-router-dom';
import { PanelCard } from '@rbgrid/design-system';
import { useCapabilities, ConfigDomain } from '../core';
import { ShadowConfigEditor } from '../components/settings/ShadowConfigEditor';
import { GroupsPanel } from '../components/settings/GroupsPanel';
import { GridPanel } from '../components/settings/GridPanel';
import { SettingsReconcileBar } from '../components/settings/SettingsReconcileBar';
import { TelemetryPanel } from '../components/settings/TelemetryPanel';
import { EvPanel } from '../components/settings/EvPanel';
import { BackupPanel } from '../components/settings/BackupPanel';
import { HubFirmwarePanel } from '../components/settings/HubFirmwarePanel';
import { SecurityPanel } from '../components/settings/SecurityPanel';
import { NetworkPanel } from '../components/settings/NetworkPanel';
import { HubAccessWarning } from '../components/common/HubAccessWarning';
import { ApPasswordPanel } from '../components/settings/ApPasswordPanel';
import { isDemo } from '../services/connection';
import { fwAtLeast } from '../entities/hubVersion';
import { useHubVersion } from '../hooks/useHubVersion';
import { RELEASE_HIDDEN } from '../constants/release';
import { HomeAssistantPanel } from '../components/settings/HomeAssistantPanel';
import { AdminPanel } from '../components/settings/AdminPanel';
import { SiteProfilePanel } from '../components/settings/SiteProfilePanel';
import { CorsPanel } from '../components/settings/CorsPanel';
import { TimePanel } from '../components/settings/TimePanel';
import { InterfacePanel } from '../components/settings/InterfacePanel';
import { t } from '../i18n';

interface DomainPanel {
  domain: ConfigDomain;
  /** Phrase key of the panel title (resolved at render). */
  title: string;
  /** Capability flag gating this panel (undefined = always shown). */
  cap?: 'mqttConfig' | 'telemetryConfigPost';
  /** No working POST for this domain on the device (DirectTransport write table) → view only. */
  readOnly?: boolean;
}

/** Anchors land below the sticky «Нет несохранённых изменений» bar (≈ 60 px), not under it. */
const ANCHOR_MARGIN = 88;

const PANELS: DomainPanel[] = [
  { domain: 'system', title: 'config.panelSystem' },
];

export const ConfigurationPage: React.FC = () => {
  const caps = useCapabilities();
  const hubVersion = useHubVersion();
  // v1.1.2 (operator 03.11 via root): hub ≥ 1.1.2 that lists the endpoint; hidden until root's .139 run.
  const showAp = !RELEASE_HIDDEN.apPassword && (isDemo() || (caps.endpoints.some((e) => e.path === '/api/system/ap') && fwAtLeast(hubVersion, [1, 1, 2]) === true));
  const { hash } = useLocation();
  // Deep link from the Overview («Сценарий: … · изменить» → /config#site-profile).
  // Panels load after the first paint and push each other down: retry until the target settles
  // (3φ rerun H3P-20: #grid / #interface stayed at the top). The hash also arrives as the second
  // «#…» of a HashRouter URL («#/config#interface»).
  useEffect(() => {
    const id = (hash || '').slice(1) || window.location.hash.split('#')[2] || '';
    if (!id) return undefined;
    const go = () => document.getElementById(id)?.scrollIntoView({ block: 'start' });
    const timers = [50, 500, 1200, 2500].map((ms) => setTimeout(go, ms));
    return () => timers.forEach(clearTimeout);
  }, [hash]);
  const panels = PANELS.filter((p) => !p.cap || caps.flags[p.cap]);
  const navigate = useNavigate();

  const grid2 = { display: 'grid', gap: 2, gridTemplateColumns: { xs: 'minmax(0, 1fr)', md: 'minmax(0, 1fr) minmax(0, 1fr)' } } as const;
  const wide = { gridColumn: { md: '1 / -1' } } as const;

  // Groups by meaning (operator 18.10), split by a line, no headings:
  // 1 the site · 2 language & time · 3 metering · 4 connectivity · 5 access · 6 maintenance.
  return (
    <Box>
      <Typography variant="h4" fontWeight={800} sx={{ mb: 2.5 }}>{t('config.title')}</Typography>
      <SettingsReconcileBar />

      {/* 1 · The site: what the installation is. */}
      <Box sx={grid2}>
        <Box sx={wide}><SiteProfilePanel /></Box>
        {panels.map((p) => (
          // id = deep-link anchor (/config#system …)
          <Box key={p.domain} id={p.domain} sx={{ scrollMarginTop: ANCHOR_MARGIN }}>
            <PanelCard title={t(p.title)}>
              <ShadowConfigEditor domain={p.domain} readOnly={p.readOnly} />
            </PanelCard>
          </Box>
        ))}
        <GridPanel />
        <GroupsPanel />
        <EvPanel anchorMargin={ANCHOR_MARGIN} />
      </Box>

      {/* 2 · Language and time. */}
      <Divider sx={{ my: 3 }} />
      <Box sx={grid2}>
        <Box id="interface" sx={{ scrollMarginTop: ANCHOR_MARGIN }}><InterfacePanel /></Box>
        <Box id="time" sx={{ scrollMarginTop: ANCHOR_MARGIN }}><TimePanel /></Box>
      </Box>

      {/* 3 · Metering: tariffs and points (own pages — HEU-16), the live data stream. */}
      <Divider sx={{ my: 3 }} />
      <Box sx={grid2}>
        <Box id="tariff" sx={{ scrollMarginTop: ANCHOR_MARGIN }}>
          <PanelCard title={t('config.panelTariff')}>
            <Typography variant="body2" color="text.secondary" sx={{ mb: 1.5 }}>{t('config.tariffElsewhere')}</Typography>
            <Button variant="outlined" size="small" onClick={() => navigate('/tariffs')}>{t('config.openTariffs')}</Button>
          </PanelCard>
        </Box>
        <Box id="units" sx={{ scrollMarginTop: ANCHOR_MARGIN }}>
          <PanelCard title={t('config.panelUnits')}>
            <Typography variant="body2" color="text.secondary" sx={{ mb: 1.5 }}>{t('config.unitsElsewhere')}</Typography>
            <Button variant="outlined" size="small" onClick={() => navigate('/devices')}>{t('config.openPoints')}</Button>
          </PanelCard>
        </Box>
        <Box id="telemetry" sx={{ scrollMarginTop: ANCHOR_MARGIN }}><TelemetryPanel /></Box>
      </Box>

      {/* 4 · Connectivity: Wi-Fi, MQTT / Home Assistant. */}
      <Divider sx={{ my: 3 }} />
      <Box sx={grid2}>
        {showAp && <Box sx={wide}><HubAccessWarning sx={{ mb: 0 }} /></Box>}
        <Box sx={wide}><NetworkPanel /></Box>
        {/* The hub's own access point password (bf4621e4) — where the hub lists the endpoint. */}
        {/* v1.1.2 (operator 03.11 via root): hub ≥ 1.1.2 that lists the endpoint; hidden until root's .139 run. */}
        {showAp && <Box id="hub-ap" sx={{ scrollMarginTop: ANCHOR_MARGIN }}><ApPasswordPanel /></Box>}
        {caps.flags.mqttConfig && <Box sx={wide}><HomeAssistantPanel /></Box>}
      </Box>

      {/* 5 · Access: API token, browser access (CORS). */}
      {caps.flags.auth && (
        <>
          <Divider sx={{ my: 3 }} />
          <Box sx={grid2}>
            <Box id="security" sx={{ scrollMarginTop: ANCHOR_MARGIN }}><SecurityPanel /></Box>
            <CorsPanel />
          </Box>
        </>
      )}

      {/* 6 · Maintenance: restore point, hub firmware, restart / factory reset. */}
      <Divider sx={{ my: 3 }} />
      <Box sx={grid2}>
        <BackupPanel />
        <HubFirmwarePanel />
        <AdminPanel />
      </Box>
    </Box>
  );
};
