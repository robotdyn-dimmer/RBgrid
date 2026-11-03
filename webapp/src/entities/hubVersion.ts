/**
 * The hub's firmware version (GET /api/system/version `version`, e.g. «1.1.2», «v1.1.2-dirty») against
 * a release that brought a feature. null = unknown (no version, a dev build, the demo) — callers
 * decide what unknown means for them.
 */
export function fwAtLeast(version: string | undefined | null, min: [number, number, number]): boolean | null {
  const m = /(\d+)\.(\d+)\.(\d+)/.exec(String(version ?? ''));
  if (!m) return null;
  const v = m.slice(1).map(Number);
  for (let i = 0; i < 3; i++) if (v[i] !== min[i]) return v[i] > min[i];
  return true;
}
