import { RELEASE_HIDDEN } from '../../constants/release';

/**
 * Can this hub run remote nodes? fw 1.1.7 says so in GET /api/v1/espnow `keys`. A hub before 1.1.7
 * sends no `keys`: its release images had none, so only a preview build offers nodes there.
 */
export function espnowAvailable(keys: boolean | undefined): boolean {
  return keys === true || (keys === undefined && !RELEASE_HIDDEN.espnow);
}
