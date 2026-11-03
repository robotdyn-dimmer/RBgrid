/**
 * Hub firmware image checks before an OTA upload — read from the .bin itself (ESP-IDF app image):
 * byte 0 = 0xE9, chip id at offset 12, esp_app_desc_t at offset 32 (magic 0xABCD5432) with
 * version @48, project_name @80, time @112, date @128, idf_ver @144 (32/32/16/16/32 bytes).
 * The hub reports the same fields in GET /api/system/version, so the app shows what is installed
 * vs what is about to be, and refuses another project or chip before anything is sent.
 */
export interface AppImage {
  chipId: number;
  version: string;
  project: string;
  /** «Oct 18 2026 08:14:20», the same shape as the hub's build_date. */
  built: string;
  idf: string;
}
export type ImageProblem = 'notImage' | 'noDesc' | 'project' | 'chip';

const str = (b: Uint8Array, off: number, len: number) => {
  let end = off;
  while (end < off + len && end < b.length && b[end] !== 0) end++;
  return String.fromCharCode(...Array.from(b.subarray(off, end)));
};

export function parseAppImage(buf: ArrayBuffer): AppImage | ImageProblem {
  const b = new Uint8Array(buf);
  if (b.length < 176 || b[0] !== 0xe9) return 'notImage';
  const dv = new DataView(buf);
  if (dv.getUint32(32, true) !== 0xabcd5432) return 'noDesc';
  return {
    chipId: dv.getUint16(12, true),
    version: str(b, 48, 32),
    project: str(b, 80, 32),
    built: `${str(b, 128, 16)} ${str(b, 112, 16)}`.trim(),
    idf: str(b, 144, 32),
  };
}

/** ESP-IDF chip ids by the schema's hw_profile prefix; unknown profile → not checked. */
const CHIP_BY_PROFILE: [string, number][] = [['esp32s3', 9], ['esp32s2', 2], ['esp32c3', 5], ['esp32c6', 13], ['esp32', 0]];
export function chipOfProfile(hwProfile: string | null | undefined): number | null {
  const p = (hwProfile ?? '').toLowerCase().replace(/-/g, '');
  return CHIP_BY_PROFILE.find(([k]) => p.startsWith(k))?.[1] ?? null;
}

/** Why this image must not go to this hub (null = fine). */
export function imageProblem(img: AppImage, hub: { project?: string; hwProfile?: string | null }): ImageProblem | null {
  if (hub.project && img.project && img.project !== hub.project) return 'project';
  const chip = chipOfProfile(hub.hwProfile);
  if (chip != null && img.chipId !== chip) return 'chip';
  return null;
}

/** The hub already runs exactly this build (same version, build date/time, IDF). */
export const sameBuild = (img: AppImage, hub: { version?: string; build_date?: string; idf?: string }): boolean =>
  !!hub.build_date && img.version === hub.version && img.built === hub.build_date && (!hub.idf || img.idf === hub.idf);

/** What the hub's event log says about the new image after it came back. */
export function otaVerdict(messages: string[]): 'confirmed' | 'rolledBack' | null {
  if (messages.some((m) => /ROLLED BACK/i.test(m))) return 'rolledBack';
  if (messages.some((m) => /new image confirmed valid/i.test(m))) return 'confirmed';
  return null;
}
