/**
 * Motion / eco helpers for the animated home schema (spec §10.5, review 11 CPU finding:
 * the SMIL dots cost 22–26 % main-thread at 4× throttle on a kiosk / budget phone).
 *
 *  - {@link usePrefersReducedMotion}: live `prefers-reduced-motion: reduce` (reacts to changes).
 *    Under reduce the skeleton is generated WITHOUT any `<animateMotion>`.
 *  - {@link useSmilEco}: pauses the SVG's SMIL timeline while the tab is hidden or the schema
 *    is scrolled off-screen, and resumes it when it is visible again.
 *
 * Both are jsdom/SSR safe (no matchMedia / IntersectionObserver / pauseAnimations → no-op).
 */
import { RefObject, useEffect, useState } from 'react';

const RM_QUERY = '(prefers-reduced-motion: reduce)';

function mql(): MediaQueryList | null {
  if (typeof window === 'undefined' || typeof window.matchMedia !== 'function') return null;
  try { return window.matchMedia(RM_QUERY); } catch { return null; }
}

export function usePrefersReducedMotion(): boolean {
  const [reduce, setReduce] = useState<boolean>(() => mql()?.matches ?? false);
  useEffect(() => {
    const m = mql();
    if (!m) return undefined;
    const on = () => setReduce(m.matches);
    on();
    if (typeof m.addEventListener === 'function') {
      m.addEventListener('change', on);
      return () => m.removeEventListener('change', on);
    }
    // Safari < 14
    m.addListener?.(on);
    return () => m.removeListener?.(on);
  }, []);
  return reduce;
}

type SmilSvg = SVGSVGElement & { pauseAnimations?: () => void; unpauseAnimations?: () => void; animationsPaused?: () => boolean };

/**
 * Pause/resume the SMIL timeline of the `<svg>` inside `host` by page visibility and
 * viewport intersection. `dep` must change whenever the svg node is replaced (skeleton
 * rebuild / layout switch) so the observers re-attach to the new node.
 */
export function useSmilEco(hostRef: RefObject<HTMLElement>, dep: unknown): void {
  useEffect(() => {
    const svg = hostRef.current?.querySelector('svg') as SmilSvg | null;
    if (!svg || typeof svg.pauseAnimations !== 'function' || typeof svg.unpauseAnimations !== 'function') return undefined;

    let onScreen = true;
    const apply = () => {
      const run = onScreen && !(typeof document !== 'undefined' && document.hidden);
      const paused = svg.animationsPaused?.() ?? false;
      if (run && paused) svg.unpauseAnimations!();
      else if (!run && !paused) svg.pauseAnimations!();
    };

    document.addEventListener('visibilitychange', apply);
    let io: IntersectionObserver | null = null;
    if (typeof window !== 'undefined' && typeof window.IntersectionObserver === 'function') {
      io = new window.IntersectionObserver((entries) => {
        const e = entries[entries.length - 1];
        if (e) { onScreen = e.isIntersecting; apply(); }
      });
      io.observe(svg);
    }
    apply();
    return () => {
      document.removeEventListener('visibilitychange', apply);
      io?.disconnect();
    };
  }, [hostRef, dep]);
}
