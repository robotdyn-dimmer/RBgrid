/**
 * useSiteProfile — the Overview scenario (spec §3 / gap G1): home | industrial | ev.
 *
 * G1 is LIVE on the contract (firmware note g-fields-live, canon 8631daf6, 2026-10-12):
 * `system.site_profile` round-trips through GET/POST /api/system/system-config and
 * live-applies (no reboot — the device's `requires_reboot:true` on this field is ignored
 * per root). So the value is sourced from the `system` config shadow, not a local store:
 *   - read: shadow `reported` (device truth);
 *   - write: DeviceSession.applyLiveField — the reported doc + this one field, re-read after.
 * localStorage is kept only as an instant-paint cache for the first frame before the GET
 * lands (and as the offline fallback), never as the source of truth.
 */
import { useCallback, useEffect, useRef, useState } from 'react';
import { useLocalStorage } from './useLocalStorage';
import { useConfigShadow, useDeviceSession } from '../core';
import { SiteProfile } from '../types/config';
import { t } from '../i18n';

const KEY = 'rbgrid.site_profile';
/** A layout tried in THIS browser only, without writing the hub (UX audit INP-18: an integrator
 *  on a read-only hub could not show a customer the «Предприятие» desk). */
const PREVIEW_KEY = 'rbgrid.site_profile.preview';
const VALID: SiteProfile[] = ['home', 'industrial', 'ev'];
const isValid = (v: unknown): v is SiteProfile =>
  typeof v === 'string' && (VALID as string[]).includes(v);

export interface SiteProfileState {
  /** Where the shown value came from: the device config, or the local cache/default. */
  /** 'loading' until the hub answered once — not «no data from the device» (W191-18). */
  source: 'device' | 'local' | 'loading';
  /** A change is being pushed to the device. */
  saving: boolean;
  /** Last write error (Russian, user-facing), or null. */
  error: string | null;
  /** The device's own scenario (what everyone else sees). */
  deviceProfile: SiteProfile;
  /** A local preview is shown instead of the device's scenario. */
  preview: SiteProfile | null;
  setPreview: (p: SiteProfile | null) => void;
}

/**
 * The device's scenario. Reading is free; WRITING changes the layout for everyone who opens
 * this hub, so only Настройки/Мастер call the setter, after an explicit confirmation.
 * The write is a live single-field apply (DeviceSession.applyLiveField): it never sends the
 * user's other pending system edits and never goes through the reboot path (G1 live-apply).
 */
export function useSiteProfile(): [SiteProfile, (p: SiteProfile) => Promise<boolean>, SiteProfileState] {
  const session = useDeviceSession();
  const sys = useConfigShadow('system');
  const [cached, setCached] = useLocalStorage<SiteProfile>(KEY, 'home');
  const [previewRaw, setPreviewRaw] = useLocalStorage<SiteProfile | ''>(PREVIEW_KEY, '');
  const preview = isValid(previewRaw) ? previewRaw : null;
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);

  // Pull the device's system-config once so the Overview reflects the configured profile
  // without a manual Settings visit. Guarded so the per-notify re-run doesn't refetch.
  const didLoad = useRef(false);
  const [loadFailed, setLoadFailed] = useState(false);
  useEffect(() => {
    if (didLoad.current) return;
    didLoad.current = true;
    if (Object.keys(session.current.get('system').reported).length === 0) {
      void session.loadDomain('system').catch(() => { setLoadFailed(true); /* offline → cache/default */ });
    }
  }, [session]);

  // Device truth first (reported), not the local desired draft.
  const fromDevice = sys.reported.site_profile;
  const deviceProfile: SiteProfile = isValid(fromDevice) ? fromDevice : isValid(cached) ? cached : 'home';
  const profile: SiteProfile = preview ?? deviceProfile;
  const answered = Object.keys(sys.reported).length > 0;
  const source: SiteProfileState['source'] = isValid(fromDevice) ? 'device' : answered || loadFailed ? 'local' : 'loading';

  // Keep the instant-paint cache in step with the device.
  useEffect(() => {
    if (isValid(fromDevice) && fromDevice !== cached) setCached(fromDevice);
  }, [fromDevice, cached, setCached]);

  const set = useCallback(
    async (p: SiteProfile): Promise<boolean> => {
      if (!isValid(p) || p === deviceProfile) { setPreviewRaw(''); return true; }
      setSaving(true);
      setError(null);
      try {
        await session.applyLiveField('system', 'site_profile', p);
        setPreviewRaw('');
        return true;
      } catch (e) {
        // Say WHY (INP-18: «Не удалось сохранить» with no reason).
        const status = (e as { status?: number })?.status;
        setError(status === 401 || status === 403
          ? t('scenario.errorToken')
          : status == null ? t('scenario.errorNetwork')
            : t('scenario.errorRejected', { status }));
        return false;
      } finally {
        setSaving(false);
      }
    },
    [session, deviceProfile, setPreviewRaw],
  );

  const setPreview = useCallback((p: SiteProfile | null) => setPreviewRaw(p && p !== deviceProfile ? p : ''), [setPreviewRaw, deviceProfile]);
  return [profile, set, { source, saving, error, deviceProfile, preview, setPreview }];
}

/** Scenario labels — getters resolve the phrase at read time (the language can switch at runtime). */
export const SITE_PROFILE_LABEL: Record<SiteProfile, string> = {
  get home() { return t('scenario.home'); },
  get industrial() { return t('scenario.industrial'); },
  get ev() { return t('scenario.ev'); },
};
