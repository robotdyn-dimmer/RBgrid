/**
 * i18n core — phrase files compiled per language (operator 2026-10-13), English built in and the
 * interface language taken from the HUB (operator 18.10: one device, one user, one language).
 *
 * - Phrase files: src/i18n/locales/<lang>/<namespace>.json — flat {"key": "text"} per namespace.
 *   `npm run i18n:index` regenerates locales/<lang>/index.ts and the language repository
 *   public/static/lang/ (index.json + one <lang>.json pack per language other than English).
 * - English ships in the main bundle. Any other language is ONE pack loaded on demand:
 *   this browser's cache (only ONE alternative language is kept) → the hub's file system when the
 *   app is served by the hub → the repository (REACT_APP_LANG_REPO, default <app>/static/lang — the hub serves a kit's static/ only). A pack
 *   fetched from the repository by a hub-served app is stored on the hub (replacing the previous).
 * - Which language: the hub's system.language (DeviceLanguage). Not set → the browser's language
 *   if a pack exists → English. `?lang=xx` in the URL overrides for this tab (review / screenshots).
 * - Adding a language = a phrase folder + `npm run i18n:index` + publishing public/static/lang/ — no
 *   firmware release (the hub stores a code, never text). Keys missing in an older pack read English.
 * - t(key, vars): "{name}" interpolation; plurals via Intl.PluralRules — keys "<key>_one",
 *   "_few", "_many", "_other", selected by vars.count. Missing key → English → the key itself.
 * - Numbers/dates: intlLocale() = the viewer's FORMAT choice («Настройки → Интерфейс») or the
 *   language's; plural forms always follow the LANGUAGE.
 */
import enCatalog from './locales/en';

/** ISO 639-1 code, lower case ("en", "ru", "de"…). */
export type Lang = string;
export type Catalog = Record<string, string>;
type Vars = Record<string, string | number | null | undefined>;

export const DEFAULT_LANG: Lang = 'en';
export interface LangInfo {
  code: Lang;
  /** The language in its own name («Русский»). */
  label: string;
  intl: string;
  /** Pack version (hash of its phrases). */
  hash?: string;
  /** Pack file in the repository (absent for the built-in language). */
  file?: string;
  bytes?: number;
}
const BUILTIN: LangInfo = { code: 'en', label: 'English', intl: 'en-GB' };

/** A stored language pack (repository / hub / this browser). */
export interface LangPack { code: Lang; label: string; intl: string; hash?: string; phrases: Catalog }

/** Where a hub-served app keeps its one alternative language (set by DeviceLanguage). */
export interface HubLangStore {
  get(code: Lang): Promise<LangPack | null>;
  put(pack: LangPack): Promise<void>;
}

const PACK_KEY = 'rbgrid.langpack';     // the ONE alternative language kept in this browser
const HUB_LANG_KEY = 'rbgrid.hubLang';  // the hub's language as last seen (first paint, no flash)
const FORMAT_KEY = 'rbgrid.format';

/** Number/date formats a viewer can pick; 'auto' = the interface language's. */
export const FORMATS: string[] = ['auto', 'ru-RU', 'en-GB', 'en-US', 'de-DE'];
let numFormat = 'auto';
try { const f = localStorage.getItem(FORMAT_KEY); if (f && FORMATS.includes(f)) numFormat = f; } catch { /* storage unavailable */ }

const repoBase = (): string => (process.env.REACT_APP_LANG_REPO || `${process.env.PUBLIC_URL || ''}/static/lang`).replace(/\/$/, '');
const registry = new Map<Lang, LangPack>([['en', { ...BUILTIN, phrases: enCatalog }]]);
let available: LangInfo[] = [BUILTIN];
let hubStore: HubLangStore | null = null;
let lang: Lang = DEFAULT_LANG;
let catalog: Catalog = enCatalog;
let intlOfLang = BUILTIN.intl;
const listeners = new Set<() => void>();
const emit = () => listeners.forEach((l) => l());

export const isLangCode = (v: unknown): v is Lang => typeof v === 'string' && /^[a-z]{2}$/.test(v);

/** Hub code ("EN", "ru", "" …) → a language code, or null when not set / not a code. */
export function langFromDevice(code: unknown): Lang | null {
  const c = typeof code === 'string' ? code.trim().toLowerCase() : '';
  return isLangCode(c) ? c : null;
}

/** Languages there are packs for (repository index), the built-in one first. */
export const getLanguages = (): LangInfo[] => available;

/** A pack known without the network (tests register Russian; a loaded pack registers itself). */
export function registerCatalog(pack: LangPack): void { registry.set(pack.code, pack); }

/** Set by DeviceLanguage when the app is served by a hub that can store a language pack. */
export function setHubLangStore(store: HubLangStore | null): void { hubStore = store; }

function readCachedPack(): LangPack | null {
  try { const p = JSON.parse(localStorage.getItem(PACK_KEY) || 'null'); return p && isLangCode(p.code) && p.phrases ? p : null; } catch { return null; }
}
function cachePack(pack: LangPack): void {
  try { localStorage.setItem(PACK_KEY, JSON.stringify(pack)); } catch { /* quota / storage off */ }
}

async function fetchJson<T>(url: string): Promise<T | null> {
  try {
    const r = await fetch(url, { cache: 'no-cache' });
    return r.ok ? ((await r.json()) as T) : null;
  } catch { return null; }
}

/** Read the repository index (which languages exist); a stale cached pack is dropped. */
export async function loadLanguageIndex(): Promise<LangInfo[]> {
  const idx = await fetchJson<{ languages?: LangInfo[] }>(`${repoBase()}/index.json`);
  const list = (idx?.languages ?? []).filter((l) => isLangCode(l.code) && l.label);
  if (list.length) {
    available = [BUILTIN, ...list.filter((l) => l.code !== BUILTIN.code)];
    const cached = readCachedPack();
    const fresh = cached && list.find((l) => l.code === cached.code)?.hash;
    if (cached && fresh && cached.hash !== fresh) {
      try { localStorage.removeItem(PACK_KEY); } catch { /* ignore */ }
      if (registry.get(cached.code)?.hash === cached.hash) registry.delete(cached.code);
    }
    emit();
  }
  return available;
}

async function loadPack(code: Lang): Promise<LangPack> {
  const known = registry.get(code);
  if (known) return known;
  const cached = readCachedPack();
  if (cached?.code === code) { registry.set(code, cached); return cached; }
  let pack = hubStore ? await hubStore.get(code).catch(() => null) : null;
  if (!pack) {
    pack = await fetchJson<LangPack>(`${repoBase()}/${code}.json`);
    if (pack && hubStore) void hubStore.put(pack).catch(() => undefined);
  }
  if (!pack || !pack.phrases) throw new Error(`no language pack «${code}»`);
  cachePack(pack); // replaces the previous alternative language
  registry.set(code, pack);
  return pack;
}

export const getLang = (): Lang => lang;
/** The interface language's own Intl locale (plural rules, the «auto» format). */
export const langIntl = (): string => intlOfLang;
/** Locale for numbers and dates: the viewer's format choice, else the language's. */
export const intlLocale = (): string => (numFormat !== 'auto' ? numFormat : langIntl());
/** Dates: month and weekday names in the INTERFACE language, the rest (12/24 h, region) by the
 *  chosen format — EN with «de-DE» read «16. Okt.» (3φ audit L3P-20); now «16 Oct, 17:05». */
export const dateLocale = (): string => {
  if (numFormat === 'auto') return langIntl();
  try {
    const region = new Intl.Locale(numFormat).region;
    const hc = (new Intl.DateTimeFormat(numFormat, { hour: 'numeric' }).resolvedOptions() as { hourCycle?: string }).hourCycle;
    const base = langIntl().split('-')[0];
    return `${base}${region ? `-${region}` : ''}${hc ? `-u-hc-${hc}` : ''}`;
  } catch { return numFormat; }
};
export const getFormat = (): string => numFormat;

/** Pick the number/date format ('auto' = follow the language); persisted in this browser. */
export function setFormat(next: string): void {
  if (!FORMATS.includes(next)) return;
  numFormat = next;
  try { if (next === 'auto') localStorage.removeItem(FORMAT_KEY); else localStorage.setItem(FORMAT_KEY, next); } catch { /* ignore */ }
  emit();
}

function activate(pack: LangPack): void {
  lang = pack.code;
  catalog = pack.phrases;
  intlOfLang = pack.intl || pack.code;
  if (typeof document !== 'undefined') document.documentElement.lang = pack.code;
  emit();
}

/** Load (on demand) and activate a language; English when the pack can't be had. */
export async function setLang(next: Lang): Promise<void> {
  if (!isLangCode(next)) next = DEFAULT_LANG;
  try { activate(await loadPack(next)); } catch { activate(registry.get(DEFAULT_LANG)!); }
}

/** The hub's language (DeviceLanguage): remembered for the next first paint, then applied. */
export async function applyHubLang(code: Lang | null): Promise<void> {
  try { if (code) localStorage.setItem(HUB_LANG_KEY, code); else localStorage.removeItem(HUB_LANG_KEY); } catch { /* ignore */ }
  if (urlLang()) return; // a tab override stays
  const want = code ?? browserLang();
  if (want !== lang) await setLang(want);
}

const urlLang = (): Lang | null => {
  try { const v = new URLSearchParams(window.location.search).get('lang'); return isLangCode(v) ? v : null; } catch { return null; }
};
export const browserLang = (): Lang => {
  const nav = typeof navigator !== 'undefined' ? navigator.language?.slice(0, 2).toLowerCase() : '';
  return isLangCode(nav) ? nav : DEFAULT_LANG;
};

/** First paint: URL override → the hub's language as last seen → the browser's → English. */
export function initialLang(): Lang {
  const u = urlLang();
  if (u) return u;
  try { const h = localStorage.getItem(HUB_LANG_KEY); if (isLangCode(h)) return h; } catch { /* ignore */ }
  return browserLang();
}

/** Activate the language before the first render (no flash); a missing pack never blocks. */
export async function initI18n(): Promise<void> {
  void loadLanguageIndex();
  await setLang(initialLang());
}

/** Tests: a language active synchronously, without the network. */
export function useCatalogForTests(pack: LangPack): void { registerCatalog(pack); activate(pack); }

export function onLangChange(cb: () => void): () => void {
  listeners.add(cb);
  return () => { listeners.delete(cb); };
}

function lookup(key: string): string | undefined {
  return catalog[key] ?? enCatalog[key];
}

function format(s: string, vars?: Vars): string {
  if (!vars) return s;
  // A var passed but empty (null/undefined) reads «—»; a var not passed at all stays visible as
  // "{name}" so a missing argument shows up in review.
  return s.replace(/\{(\w+)\}/g, (m, k: string) => (!(k in vars) ? m : vars[k] == null ? '—' : String(vars[k])));
}

/** Translate a phrase key. */
export function t(key: string, vars?: Vars): string {
  if (vars && typeof vars.count === 'number') {
    const form = new Intl.PluralRules(langIntl()).select(vars.count);
    const plural = catalog[`${key}_${form}`] ?? catalog[`${key}_other`]
      ?? enCatalog[`${key}_${new Intl.PluralRules('en-GB').select(vars.count)}`] ?? enCatalog[`${key}_other`];
    if (plural) return format(plural, vars);
  }
  return format(lookup(key) ?? key, vars);
}

/** Phrase for a firmware CODE (role, zone, status…): "<ns>.<CODE>", falls back to the code. */
export function tCode(ns: string, code: string | null | undefined): string {
  if (!code) return '';
  const s = lookup(`${ns}.${code}`);
  return s ?? code;
}
