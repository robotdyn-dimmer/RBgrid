/**
 * Success text for a write: in demo mode nothing reaches a hub, so every «сохранено на приборе»
 * reads «Сохранено (демо — только в этом браузере)» instead (UX audit HEU-05/PNT-07).
 */
import { t } from '../i18n';
import { isDemo } from './connection';

export const savedText = (key: string, vars?: Record<string, string | number | null | undefined>): string =>
  (isDemo() ? t('common.savedDemo') : t(key, vars));
