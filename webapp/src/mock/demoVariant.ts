/**
 * Demo variants (operator 20.10: a public demo «с вариантами моков») — one synthetic site per
 * variant, picked in the demo's own switcher or by link (`?demo=business`, or the site's names
 * `?scenario=heatpump-ev|flat|solar|workshop|carpark`), kept per browser.
 *
 *  heatpump-ev   a three-phase house with a heat pump and an EV charger, two-zone tariff (default
 *                since 02.11 — site brief: the site doesn't lead with solar)
 *  home          a home with solar, single-phase input
 *  home-nosolar  the same home without generation (the solar line is a garage)
 *  business      a small business: a three-phase STD-U3I3 input and production lines
 *  ev            a car park: charging stations and the building
 *  us            120/240 V split-phase home (USD) — preview builds only (split phase is deferred
 *                from v1.1.0, constants/release)
 *
 * The older per-knob keys (`rbgrid.demo.noGen`, `rbgrid.demo.region`) still pick home-nosolar / us.
 */
import { RELEASE_HIDDEN } from '../constants/release';

export type DemoVariantId = 'heatpump-ev' | 'home' | 'home-nosolar' | 'business' | 'ev' | 'us';
export type DemoProfile = 'home' | 'industrial' | 'ev';

export interface DemoVariant {
  id: DemoVariantId;
  /** The scenario the Overview opens with (system.site_profile). */
  profile: DemoProfile;
  /** A generation line on site. */
  solar: boolean;
  /** 120/240 V split-phase service, USD. */
  us: boolean;
  /** A three-phase input (STD-U3I3, per-phase readings). */
  threePhase: boolean;
}

const VARIANTS: Record<DemoVariantId, DemoVariant> = {
  'heatpump-ev': { id: 'heatpump-ev', profile: 'home', solar: false, us: false, threePhase: true },
  home: { id: 'home', profile: 'home', solar: true, us: false, threePhase: false },
  'home-nosolar': { id: 'home-nosolar', profile: 'home', solar: false, us: false, threePhase: false },
  business: { id: 'business', profile: 'industrial', solar: false, us: false, threePhase: true },
  ev: { id: 'ev', profile: 'ev', solar: false, us: false, threePhase: false },
  us: { id: 'us', profile: 'home', solar: true, us: true, threePhase: false },
};

const KEY = 'rbgrid.demo.variant';
const isId = (v: unknown): v is DemoVariantId => typeof v === 'string' && v in VARIANTS;

/** The site's scenario names (rbgrid.io links, site team 02.11) → the demo's sites. */
const SCENARIOS: Record<string, DemoVariantId> = {
  'heatpump-ev': 'heatpump-ev', flat: 'home-nosolar', solar: 'home', workshop: 'business', carpark: 'ev',
};

/** The variants this build offers, in menu order. */
export function demoVariantIds(): DemoVariantId[] {
  const all: DemoVariantId[] = ['heatpump-ev', 'home', 'home-nosolar', 'business', 'ev'];
  const ids: DemoVariantId[] = RELEASE_HIDDEN.splitPhase ? all : [...all, 'us'];
  // A build may publish only some sites: REACT_APP_DEMO_SITES=home,ev (the three-phase workshop was
  // held 20.10 until the operator opened positioning gate Q-20261017-01 on 21.10).
  const only = (process.env.REACT_APP_DEMO_SITES ?? '').split(',').map((x) => x.trim()).filter((x): x is DemoVariantId => isId(x));
  return only.length ? ids.filter((id) => only.includes(id)) : ids;
}

const offered = (id: DemoVariantId): boolean => demoVariantIds().includes(id);

/** The site named by the address: `?demo=<id>` or `?scenario=<site's name>`. */
function linkedId(): DemoVariantId | null {
  try {
    const q = new URLSearchParams(window.location.search);
    const d = q.get('demo');
    if (isId(d) && offered(d)) return d;
    const s = SCENARIOS[q.get('scenario') ?? ''];
    if (s && offered(s)) return s;
  } catch { /* no window (tests) */ }
  return null;
}

/** The active variant: the link → this browser's choice → the older keys → the first offered. */
export function demoVariant(): DemoVariant {
  const linked = linkedId();
  if (linked) return VARIANTS[linked];
  try {
    const v = localStorage.getItem(KEY);
    if (isId(v) && offered(v)) return VARIANTS[v];
    if (localStorage.getItem('rbgrid.demo.region') === 'us' && offered('us')) return VARIANTS.us;
    if (localStorage.getItem('rbgrid.demo.noGen') === '1') return VARIANTS['home-nosolar'];
  } catch { /* storage off */ }
  return VARIANTS[demoVariantIds()[0] ?? 'home'];
}

const SCREENS: Record<string, string> = {
  overview: '/dashboard', history: '/history', economics: '/economics', events: '/events', tariffs: '/tariffs', points: '/devices',
  settings: '/config', onboarding: '/onboarding', wizard: '/onboarding',
};
const RANGES: Record<string, string> = {
  today: 'today', yesterday: 'yesterday', '7d': 'week', week: 'week', month: 'thisMonth', '30d': 'thisMonth', year: 'year',
};

/**
 * A site link may open a screen: `&screen=history&range=7d` (+ `&metric=money` for History).
 * Applied once, before the router reads the hash, and only when the address names no route yet.
 */
export function applyDemoScreenLink(): void {
  try {
    const q = new URLSearchParams(window.location.search);
    const path = SCREENS[q.get('screen') ?? ''];
    if (!path || (window.location.hash && window.location.hash !== '#/')) return;
    const h = new URLSearchParams();
    if (path === '/history') {
      const win = RANGES[q.get('range') ?? ''];
      if (win) h.set('win', win);
      const metric = q.get('metric');
      if (metric === 'money' || metric === 'energy' || metric === 'power') h.set('metric', metric);
    }
    const hs = h.toString();
    window.location.hash = `#${path}${hs ? `?${hs}` : ''}`;
  } catch { /* no window */ }
}

/**
 * Switch the demo to another site. Everything demo-side is read once per page (the units, the
 * config the browser edited), so the page reloads — with the link updated when it named one.
 */
export function setDemoVariant(id: DemoVariantId): void {
  try {
    localStorage.setItem(KEY, id);
    // A fresh site: drop what the visitor changed on the previous one (bound / removed points,
    // edited settings) — it belonged to that site.
    localStorage.removeItem('rbgrid.demo.units');
    localStorage.removeItem('rbgrid.demo.config');
  } catch { /* storage off — the link below still carries it */ }
  try {
    // A real reload every time: replace() to the same URL is only a hash navigation — the frames
    // switched (they read the variant each tick) while the units config, history and the flow
    // stayed the old site's (operator 20.10).
    const url = new URL(window.location.href);
    if (url.searchParams.has('scenario')) url.searchParams.delete('scenario');
    if ((url.searchParams.has('demo') || linkedId()) && url.searchParams.get('demo') !== id) {
      url.searchParams.set('demo', id);
      window.location.replace(url.toString());
    } else {
      window.location.reload();
    }
  } catch { /* tests */ }
}
