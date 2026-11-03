# Interface languages (phrase files)

Decision 2026-10-13: phrase files are **compiled at build time**, not parsed on the fly.
Decision 2026-10-18 (operator): **English is built into the app**; any other language is one
**language pack** loaded on demand, and the interface language **is the hub's**
(`system.language`, one device = one user). Only ONE alternative language is kept at a time.

Where a pack comes from (i18n.ts `loadPack`):
1. this browser's cache (`localStorage rbgrid.langpack`, the last alternative language only);
2. the hub's file system, when the app is served by the hub and the hub can store a pack
   (schema endpoint `/api/system/lang`, firmware pending);
3. the **language repository** `REACT_APP_LANG_REPO` (default `<app>/static/lang/` — the hub serves a kit's `static/` only): `index.json`
   lists the languages, `<code>.json` is a pack. A hub-served app stores the fetched pack on the hub.

Adding a language needs **no firmware release**: the hub stores a code, never text. Publish the
new pack to the repository; an app whose pack lacks newer keys shows them in English.

## Layout
```
src/i18n/locales/<lang>/<namespace>.json   flat {"key": "text"} — what translators edit
src/i18n/locales/<lang>/index.ts           GENERATED (npm run i18n:index) — never edit
```
In code a phrase is `t('<namespace>.<key>', { var })`. Shared namespaces: `common`, `units`,
`role`, `zone`, `shell`; screens have their own (`point`, `history`, `cards`, …).

## Rules for phrases
- Placeholders `{name}` must be identical in every language (the test checks it).
- Plurals: `key_one` / `key_few` / `key_many` / `key_other` — only the forms a language needs
  (ru: one/few/many/other, en: one/other); call `t('ns.key', { count })`.
- Device codes: `tCode('role', 'EV_CHARGING')` → phrase `role.EV_CHARGING`, unknown → the code.
- Never call `t()` at module load — keep keys in constants, resolve at render.
- Numbers/dates follow the language (`intlLocale()`: ru-RU decimal comma, en-GB point).

## Add a language (e.g. `de`)
1. Copy `locales/en/` to `locales/de/`, translate the JSON values, set `_meta.json` {label, intl}.
2. `npm run i18n:index` — writes `public/static/lang/de.json` and adds it to `public/static/lang/index.json`.
3. Publish `public/static/lang/` (the repository). The hub must accept the code in `system.language`.

## Checks (src/i18n/i18n.test.ts)
Every language covers the default catalog, no stray keys, placeholders match, every literal
`t('ns.key')` in `src/` exists, generated indexes are fresh.
