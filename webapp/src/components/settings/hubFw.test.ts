import fs from 'fs';
import path from 'path';
import { parseAppImage, imageProblem, sameBuild, chipOfProfile, otaVerdict, AppImage } from './hubFw';

// The first 512 bytes of a real hub build (build/main.bin, 2026-10-18): image header + esp_app_desc.
const head = fs.readFileSync(path.join(__dirname, '../../test/fixtures/hub-image-head.bin'));
const buf = head.buffer.slice(head.byteOffset, head.byteOffset + head.byteLength) as ArrayBuffer;

describe('hub firmware image', () => {
  it('reads version / project / build / IDF from a real image', () => {
    const img = parseAppImage(buf) as AppImage;
    expect(img).toMatchObject({ chipId: 0, version: '1.0.2', project: 'main', idf: 'v5.4.1' });
    expect(img.built).toMatch(/^Oct 18 2026 \d\d:\d\d:\d\d$/);
  });
  it('refuses what is not an app image', () => {
    expect(parseAppImage(new Uint8Array(512).buffer)).toBe('notImage');
    const b = new Uint8Array(buf.slice(0)); b[32] = 0;
    expect(parseAppImage(b.buffer)).toBe('noDesc');
  });
  it('refuses another project or chip, knows the same build', () => {
    const img = parseAppImage(buf) as AppImage;
    expect(imageProblem(img, { project: 'main', hwProfile: 'esp32-wrover-psram' })).toBeNull();
    expect(imageProblem(img, { project: 'other' })).toBe('project');
    expect(imageProblem(img, { hwProfile: 'esp32s3-n16r8' })).toBe('chip');
    expect(chipOfProfile('unknown')).toBeNull();
    expect(sameBuild(img, { version: '1.0.2', build_date: img.built, idf: 'v5.4.1' })).toBe(true);
    expect(sameBuild(img, { version: '1.0.2', build_date: 'Oct 16 2026 21:38:22' })).toBe(false);
  });
  it('reads the verdict from the hub log', () => {
    expect(otaVerdict(['ota: new image confirmed valid'])).toBe('confirmed');
    expect(otaVerdict(['FAIL ota: ROLLED BACK to ota_0'])).toBe('rolledBack');
    expect(otaVerdict(['wifi Connected'])).toBeNull();
  });
});

import { apPasswordOk } from './ApPasswordPanel';
describe('AP password (8–63 printable ASCII, root 20.10)', () => {
  it('accepts a WPA2 passphrase and refuses short, long and non-ASCII ones', () => {
    expect(apPasswordOk('correct horse')).toBe(true);
    expect(apPasswordOk('1234567')).toBe(false);
    expect(apPasswordOk('x'.repeat(64))).toBe(false);
    expect(apPasswordOk('пароль12345')).toBe(false);
  });
});
