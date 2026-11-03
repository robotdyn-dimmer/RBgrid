/**
 * AppShell — HA-style application layout: composes the @rbgrid/design-system <AppShell>
 * (brand + nav rail + content) with react-router nav state and the device/connection footer.
 */
import React, { useEffect } from 'react';
import { Outlet, useNavigate, useLocation } from 'react-router-dom';
import { Box, Typography, Button, TextField, MenuItem, useMediaQuery, useTheme } from '@mui/material';
import SpaceDashboardIcon from '@mui/icons-material/SpaceDashboard';
import RocketLaunchIcon from '@mui/icons-material/RocketLaunch';
import DeviceHubIcon from '@mui/icons-material/DeviceHub';
import ShowChartIcon from '@mui/icons-material/ShowChart';
import NotificationsActiveIcon from '@mui/icons-material/NotificationsActive';
import PaymentsIcon from '@mui/icons-material/Payments';
import RequestQuoteIcon from '@mui/icons-material/RequestQuote';
import TuneIcon from '@mui/icons-material/Tune';
import BoltIcon from '@mui/icons-material/Bolt';
import SwapHorizIcon from '@mui/icons-material/SwapHoriz';
import { AppShell as DSAppShell, NavItem } from '@rbgrid/design-system';
import { useConnection } from '../hooks/useConnection';
import { disconnect, DEMO_ONLY } from '../services/connection';
import { ThemeSwitch } from '../theme/ThemeSwitch';
import { t } from '../i18n';
import { useTelemetryEntities } from '../hooks/useTelemetryEntities';
import { LinkStatus, DeviceUnreachable, linkStateOf, LinkState } from './ConnectionStatus';
import { confirmLeave, setLeaveAsker } from './unsavedGuard';
import { useConfirm } from '../components/common/ConfirmDialog';
import { demoVariant, demoVariantIds, setDemoVariant, DemoVariantId } from '../mock/demoVariant';

// Grouped rail (operator decision 2026-06-24): Мониторинг / Управление / Настройка.
// `tab` = slot position in the mobile bottom bar (< 820 px): Обзор·Экономика·История·События — money is
// the home user's main question (economics-first; rerun 16.10 RES-24: Economics was under «Ещё»),
// «Точки» moved under «Ещё»;
// everything else goes under «Ещё».
// Labels are phrase keys (shell.*), resolved at render so a language switch applies.
const NAV: { to: string; label: string; icon: React.ReactNode; group: string; tab?: number }[] = [
  { to: 'dashboard', label: 'shell.navOverview', icon: <SpaceDashboardIcon />, group: 'shell.groupMonitoring', tab: 0 },
  { to: 'history', label: 'shell.navHistory', icon: <ShowChartIcon />, group: 'shell.groupMonitoring', tab: 2 },
  { to: 'events', label: 'shell.navEvents', icon: <NotificationsActiveIcon />, group: 'shell.groupMonitoring', tab: 3 },
  { to: 'economics', label: 'shell.navEconomics', icon: <PaymentsIcon />, group: 'shell.groupMonitoring', tab: 1 },
  { to: 'devices', label: 'shell.navPoints', icon: <DeviceHubIcon />, group: 'shell.groupManage' },
  { to: 'tariffs', label: 'shell.navTariffs', icon: <RequestQuoteIcon />, group: 'shell.groupManage' },
  { to: 'onboarding', label: 'shell.navWizard', icon: <RocketLaunchIcon />, group: 'shell.groupSetup' },
  { to: 'config', label: 'shell.navSettings', icon: <TuneIcon />, group: 'shell.groupSetup' },
];

const DeviceFooter: React.FC<{ state: LinkState; lastUpdate: number | null }> = ({ state, lastUpdate }) => {
  const { connection, demo } = useConnection();
  const label = demo ? t('shell.demoMode') : (connection?.baseUrl || t('shell.thisDevice'));
  return (
    <Box sx={{ p: 2, borderTop: 1, borderColor: 'divider' }}>
      <Typography variant="caption" color="text.secondary" display="block">
        {demo ? t('shell.demoData') : t('shell.device')}
      </Typography>
      <Typography variant="body2" fontWeight={600} noWrap title={label}>{label}</Typography>
      {/* The public demo shows several sites (mock/demoVariant); switching reloads the page. */}
      {demo && (
        <TextField
          select size="small" fullWidth label={t('demo.variant')} value={demoVariant().id} sx={{ mt: 1 }}
          onChange={async (e) => { if (await confirmLeave()) setDemoVariant(e.target.value as DemoVariantId); }}
        >
          {demoVariantIds().map((id) => <MenuItem key={id} value={id}>{t(`demo.v_${id}`)}</MenuItem>)}
        </TextField>
      )}
      <Box sx={{ mt: 0.5 }}><LinkStatus state={state} lastUpdate={lastUpdate} demo={demo} /></Box>
      {!DEMO_ONLY && (
        <Button size="small" startIcon={<SwapHorizIcon />} onClick={async () => { if (await confirmLeave()) disconnect(); }} sx={{ mt: 0.5 }}>
          {t('shell.switchDevice')}
        </Button>
      )}
    </Box>
  );
};

export const AppShell: React.FC = () => {
  const navigate = useNavigate();
  const { pathname } = useLocation();
  const { demo } = useConnection();
  const confirm = useConfirm();
  // One «Уйти без сохранения?» for the menu, the browser's Back and «Сменить» (rerun 16.10
  // HEU-R03/R05: «Остаться» / «Уйти без сохранения» — «Отменить» read as «undo my edits»).
  useEffect(() => {
    setLeaveAsker(() => confirm({ title: t('form.leaveTitle'), body: t('form.leaveBody'), confirmLabel: t('form.leave'), cancelLabel: t('form.stay'), danger: true }));
    return () => setLeaveAsker(null);
  }, [confirm]);
  const theme = useTheme();
  const mobile = useMediaQuery(theme.breakpoints.down('md'));
  // One shell-level view of the link: status line + «hub unreachable» screen (UX audit T2).
  const tel = useTelemetryEntities();
  const state = linkStateOf(tel.conn, !!tel.data);
  const unreachable = !demo && !tel.data && tel.conn === 'error';

  const nav: NavItem[] = NAV.map((item) => ({
    label: t(item.label),
    icon: item.icon,
    group: t(item.group),
    tab: item.tab !== undefined,
    tabOrder: item.tab,
    // A point page belongs to «Точки» (HEU-22: nothing was highlighted on /point/*).
    active: pathname.startsWith(`/${item.to}`) || (item.to === 'devices' && pathname.startsWith('/point/')),
    onClick: async () => {
      if (!(await confirmLeave())) return;
      navigate(item.to);
    },
  }));

  return (
    <DSAppShell
      brand="RBGrid"
      brandIcon={<BoltIcon color="primary" />}
      nav={nav}
      moreLabel={t('shell.more')}
      navLabel={t('shell.navAria')}
      moreNavLabel={t('shell.moreNavAria')}
      headerRight={mobile ? <LinkStatus state={state} lastUpdate={tel.lastUpdate} demo={demo} compact /> : undefined}
      footer={
        <>
          <ThemeSwitch />
          <DeviceFooter state={state} lastUpdate={tel.lastUpdate} />
        </>
      }
    >
      {unreachable ? <DeviceUnreachable /> : <Outlet />}
    </DSAppShell>
  );
};
