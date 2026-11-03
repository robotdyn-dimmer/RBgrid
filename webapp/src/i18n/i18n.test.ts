/**
 * Phrase-file integrity: every language covers the default catalog, every literal t('…') key
 * used in src exists, placeholders match across languages, the generated indexes are fresh.
 */
import fs from 'fs';
import path from 'path';
import { execFileSync } from 'child_process';
import ru from './locales/ru';
import en from './locales/en';
import { t, setLang, intlLocale, langFromDevice, setHubLangStore, getLang } from './i18n';

const PLURAL = /_(zero|one|two|few|many|other)$/;
const base = (k: string) => k.replace(PLURAL, '');
const keysOf = (c: Record<string, string>) => new Set(Object.keys(c).map(base));
const vars = (s: string) => (s.match(/\{\w+\}/g) ?? []).sort().join(',');

function srcFiles(dir: string, out: string[] = []): string[] {
  for (const f of fs.readdirSync(dir)) {
    const p = path.join(dir, f);
    if (fs.statSync(p).isDirectory()) { if (f !== 'locales') srcFiles(p, out); }
    else if (/\.(ts|tsx)$/.test(f) && !/\.test\.tsx?$/.test(f)) out.push(p);
  }
  return out;
}

describe('phrase catalogs', () => {
  it('en covers every ru phrase (plural forms compared by base key)', () => {
    const missing = Array.from(keysOf(ru)).filter((k) => !keysOf(en).has(k));
    expect(missing).toEqual([]);
  });

  it('no stray en phrases', () => {
    const extra = Array.from(keysOf(en)).filter((k) => !keysOf(ru).has(k));
    expect(extra).toEqual([]);
  });

  it('placeholders match between languages', () => {
    const bad = Object.keys(ru).filter((k) => !PLURAL.test(k) && en[k] !== undefined && vars(ru[k]) !== vars(en[k]));
    expect(bad).toEqual([]);
  });

  it('every literal t()/tCode() key used in src exists in ru', () => {
    const used = new Set<string>();
    for (const f of srcFiles(path.join(__dirname, '..'))) {
      const s = fs.readFileSync(f, 'utf8');
      const re = /\bt\(\s*'([a-zA-Z][\w]*\.[\w.]+)'/g;
      let m: RegExpExecArray | null;
      while ((m = re.exec(s))) used.add(m[1]);
    }
    const known = keysOf(ru);
    expect(Array.from(used).filter((k) => !known.has(k))).toEqual([]);
  });

  it('every language has _meta.json and a repository pack', () => {
    const repo = JSON.parse(fs.readFileSync(path.join(__dirname, '../../public/static/lang/index.json'), 'utf8'));
    expect(repo.builtin).toBe('en');
    expect(repo.languages.map((l: { code: string }) => l.code).sort()).toEqual(['en', 'ru']);
  });

  it('generated indexes are fresh (npm run i18n:index)', () => {
    expect(() => execFileSync('node', [path.join(__dirname, '../../scripts/i18n-index.js'), '--check'])).not.toThrow();
  });
});

describe('t()', () => {
  afterAll(() => setLang('ru'));
  it('interpolates, falls back to the key, switches language and locale', async () => {
    expect(t('shell.themeIs', { theme: 'x' })).toBe('Тема: x');
    expect(t('nope.missing')).toBe('nope.missing');
    await setLang('en');
    expect(t('shell.navOverview')).toBe('Overview');
    expect(intlLocale()).toBe('en-GB');
  });
});

describe('language packs (English built in, one alternative at a time)', () => {
  const pack = (code: string, hash: string) => ({ code, label: code.toUpperCase(), intl: `${code}-XX`, hash, phrases: { 'shell.navOverview': `${code}:overview` } });
  const realFetch = global.fetch;
  afterEach(async () => { global.fetch = realFetch; setHubLangStore(null); localStorage.clear(); await setLang('ru'); });

  it('loads a pack from the repository, keeps only the last one in this browser', async () => {
    global.fetch = jest.fn(async (url: string) => ({ ok: true, json: async () => pack(String(url).match(/(\w\w)\.json$/)![1], 'h1') })) as never;
    await setLang('de');
    expect(t('shell.navOverview')).toBe('de:overview');
    expect(t('shell.navHistory')).toBe('History'); // missing in the pack → English
    await setLang('fr');
    const kept = JSON.parse(localStorage.getItem('rbgrid.langpack')!);
    expect(kept.code).toBe('fr');
  });

  it('a hub-served app reads the pack from the hub, else fetches it and stores it there', async () => {
    const put = jest.fn(async () => undefined);
    setHubLangStore({ get: async (c) => (c === 'it' ? pack('it', 'h') : null), put });
    global.fetch = jest.fn(async () => ({ ok: true, json: async () => pack('es', 'h') })) as never;
    await setLang('it');
    expect(t('shell.navOverview')).toBe('it:overview');
    expect(put).not.toHaveBeenCalled();
    await setLang('es');
    expect(put).toHaveBeenCalledWith(expect.objectContaining({ code: 'es' }));
  });

  it('no pack anywhere → English, never a blank UI', async () => {
    global.fetch = jest.fn(async () => ({ ok: false })) as never;
    await setLang('pt');
    expect(getLang()).toBe('en');
    expect(t('shell.navOverview')).toBe('Overview');
  });
});

describe('device language code', () => {
  it('maps hub codes to supported languages, ignores the rest', () => {
    expect(langFromDevice('EN')).toBe('en');
    expect(langFromDevice('ru')).toBe('ru');
    expect(langFromDevice('DE')).toBe('de'); // any ISO 639-1 code — packs decide what exists
    expect(langFromDevice('deu')).toBeNull();
    expect(langFromDevice('')).toBeNull();
    expect(langFromDevice(undefined)).toBeNull();
  });
});
