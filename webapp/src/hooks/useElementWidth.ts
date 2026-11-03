/**
 * useElementWidth — the content-box width of an element, tracked with ResizeObserver.
 *
 * Used by the flow schemas and the inline-SVG charts to pick a layout by their OWN container
 * width (not the viewport) and to draw charts in real pixels (`viewBox = 0 0 W H`), so text
 * stays at its nominal 11–12 px at every width (review 03 §5.2: no on-screen text < 11 px).
 *
 * SSR/jsdom safe: without ResizeObserver it measures once (clientWidth, which is 0 in jsdom)
 * and falls back to `fallback`, so tests render the wide layout deterministically.
 *
 * Usage: `const [ref, width] = useElementWidth<HTMLDivElement>(760);` → `<Box ref={ref}>`.
 * The ref is a callback ref, so it also works when the measured element mounts later
 * (e.g. after a loading state) or is swapped for another node.
 */
import { useCallback, useEffect, useRef, useState } from 'react';

export function useElementWidth<T extends HTMLElement = HTMLDivElement>(
  fallback = 0,
): [(node: T | null) => void, number] {
  const [width, setWidth] = useState<number>(fallback);
  const roRef = useRef<ResizeObserver | null>(null);

  const ref = useCallback(
    (node: T | null) => {
      roRef.current?.disconnect();
      roRef.current = null;
      if (!node) return;
      const measure = (w: number) => {
        // Round to whole px: sub-pixel jitter must not re-render charts every frame.
        const next = w > 0 ? Math.round(w) : fallback;
        setWidth((prev) => (prev === next ? prev : next));
      };
      measure(node.clientWidth);
      if (typeof window === 'undefined' || typeof window.ResizeObserver === 'undefined') return;
      const ro = new window.ResizeObserver((entries) => {
        const e = entries[0];
        if (e) measure(e.contentRect.width);
      });
      ro.observe(node);
      roRef.current = ro;
    },
    [fallback],
  );

  useEffect(() => () => roRef.current?.disconnect(), []);

  return [ref, width];
}
