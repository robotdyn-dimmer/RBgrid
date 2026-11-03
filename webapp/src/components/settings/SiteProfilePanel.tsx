/**
 * SiteProfilePanel — choose the site scenario (дом / предприятие / зарядные станции).
 * It is a DEVICE setting: it changes the Overview layout for everyone who opens this hub,
 * so it lives in Настройки (not as a header toggle) and asks for confirmation (review 01/04).
 * Live-applies without reboot (G1) via useSiteProfile → DeviceSession.applyLiveField.
 */
import React, { useState } from 'react';
import {
  Alert, Box, Button, CircularProgress, Dialog, DialogActions, DialogContent, DialogTitle,
  Stack, Typography,
} from '@mui/material';
import { PanelCard } from '@rbgrid/design-system';
import { useDsTokens } from '../../theme/useDsTokens';
import { useSiteProfile, SITE_PROFILE_LABEL } from '../../hooks/useSiteProfile';
import { SiteProfile } from '../../types/config';
import { t } from '../../i18n';
import { ScenarioArt } from './ScenarioArt';

const OPTIONS: { value: SiteProfile; hint: string }[] = [
  { value: 'home', hint: 'siteProfile.hintHome' },
  { value: 'industrial', hint: 'siteProfile.hintIndustrial' },
  { value: 'ev', hint: 'siteProfile.hintEv' },
];

export const SiteProfilePanel: React.FC = () => {
  const ds = useDsTokens();
  const [, setProfile, state] = useSiteProfile();
  const [pending, setPending] = useState<SiteProfile | null>(null);

  const confirm = async () => {
    if (!pending) return;
    const ok = await setProfile(pending);
    if (ok) setPending(null);
  };

  return (
    <Box id="site-profile">
      <PanelCard title={t('siteProfile.title')}>
        <Stack spacing={1.5}>
          <Typography variant="body2" color="text.secondary">
            {t('siteProfile.intro')}
          </Typography>
          <Box sx={{ display: 'grid', gap: 1.5, gridTemplateColumns: { xs: '1fr', sm: 'repeat(3, 1fr)' } }}>
            {OPTIONS.map((o) => {
              const active = o.value === state.deviceProfile;
              return (
                <Box
                  key={o.value}
                  component="button"
                  type="button"
                  onClick={() => !active && setPending(o.value)}
                  aria-pressed={active}
                  sx={{
                    textAlign: 'left', cursor: active ? 'default' : 'pointer', p: 1.5, borderRadius: '12px',
                    bgcolor: active ? ds.primarySoft : ds.surfaceRaised,
                    border: `1px solid ${active ? ds.primary : ds.border}`, color: ds.ink, font: 'inherit',
                    '&:focus-visible': { outline: `2px solid ${ds.focus}`, outlineOffset: 2 },
                  }}
                >
                  <ScenarioArt kind={o.value} />
                  <Typography sx={{ fontWeight: 600, fontSize: 15 }}>
                    {SITE_PROFILE_LABEL[o.value]}{active ? t('siteProfile.currentSuffix') : ''}
                  </Typography>
                  <Typography sx={{ fontSize: 12, color: ds.inkMuted, mt: 0.5 }}>{t(o.hint)}</Typography>
                </Box>
              );
            })}
          </Box>
          {state.source === 'local' && (
            <Typography variant="caption" color="text.secondary">
              {t('siteProfile.localFallback')}
            </Typography>
          )}
          {state.error && <Alert severity="error">{state.error}</Alert>}
          {state.preview && (
            <Alert severity="info" action={<Button color="inherit" size="small" onClick={() => state.setPreview(null)}>{t('siteProfile.previewOff')}</Button>}>
              {t('siteProfile.previewOn', { profile: SITE_PROFILE_LABEL[state.preview] })}
            </Alert>
          )}
        </Stack>
      </PanelCard>

      <Dialog open={pending != null} onClose={state.saving ? undefined : () => setPending(null)} maxWidth="xs" fullWidth>
        <DialogTitle>{t('siteProfile.confirmTitle')}</DialogTitle>
        <DialogContent>
          <Typography>
            {t('siteProfile.confirmText', { profile: pending ? SITE_PROFILE_LABEL[pending] : '' })}
          </Typography>
          {state.error && <Alert severity="error" sx={{ mt: 2 }}>{state.error}</Alert>}
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setPending(null)} disabled={state.saving}>{t('siteProfile.cancel')}</Button>
          {/* Look first — this browser only, the hub and everyone else keep theirs (INP-18). */}
          <Button onClick={() => { if (pending) state.setPreview(pending); setPending(null); }} disabled={state.saving}>
            {t('siteProfile.previewOnly')}
          </Button>
          <Button
            variant="contained" onClick={confirm} disabled={state.saving}
            startIcon={state.saving ? <CircularProgress size={16} aria-label={t('common.loading')} /> : undefined}
          >
            {state.saving ? t('common.saving') : t('siteProfile.change')}
          </Button>
        </DialogActions>
      </Dialog>
    </Box>
  );
};
