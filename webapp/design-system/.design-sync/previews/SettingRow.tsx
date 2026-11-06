import React from 'react';
import { SettingRow } from '@rbgrid/design-system';
import { Box } from '@mui/material';

export const ConfigList = () => (
  <Box sx={{ minWidth: 320 }}>
    <SettingRow label="ntp_server" value="pool.ntp.org" />
    <SettingRow label="timezone" value="Europe/Moscow" />
    <SettingRow label="sync_interval" value="3600 s" />
    <SettingRow label="last_sync" value="2026-06-21 09:14:02" />
  </Box>
);
