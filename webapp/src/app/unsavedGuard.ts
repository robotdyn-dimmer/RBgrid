/**
 * Unsaved-changes guard (UX audit HEU-21/TAR-16): a form with edits registers itself; leaving the
 * page through the app's navigation asks first («Уйти без сохранения?»), closing / reloading the
 * tab gets the browser's own prompt. One registry instead of per-page logic (the app runs on a
 * HashRouter, which has no route blocker). The browser's Back is caught here too (rerun 16.10
 * HEU-R03: only the menu asked) — the listener is installed at module load, before the router's
 * own, so a blocked step never reaches the router.
 */
import { useEffect } from 'react';

interface Entry { discard?: () => void }
const dirty = new Map<string, Entry>();

const onBeforeUnload = (e: BeforeUnloadEvent) => {
  if (dirty.size === 0) return;
  e.preventDefault();
  e.returnValue = '';
};

function sync(): void {
  if (typeof window === 'undefined') return;
  window.removeEventListener('beforeunload', onBeforeUnload);
  if (dirty.size > 0) window.addEventListener('beforeunload', onBeforeUnload);
}

/** Mark form `id` dirty while `isDirty`; `discard` resets drafts kept outside the component. */
export function useUnsavedChanges(id: string, isDirty: boolean, discard?: () => void): void {
  useEffect(() => {
    if (isDirty) dirty.set(id, { discard }); else dirty.delete(id);
    sync();
    return () => { dirty.delete(id); sync(); };
  }, [id, isDirty, discard]);
}

export const hasUnsavedChanges = (): boolean => dirty.size > 0;

/** Leaving confirmed: drop the drafts that would otherwise survive the page (shadow edits). */
export function discardUnsaved(): void {
  dirty.forEach((e) => e.discard?.());
  dirty.clear();
  sync();
}

/** Asks «Уйти без сохранения?» — set by the app shell (it owns the confirm dialog). */
let ask: (() => Promise<boolean>) | null = null;
export function setLeaveAsker(fn: (() => Promise<boolean>) | null): void { ask = fn; }

/** Ask before an action that drops the drafts (switching device…); true = go on. */
export async function confirmLeave(): Promise<boolean> {
  if (dirty.size === 0 || !ask) return true;
  const ok = await ask();
  if (ok) discardUnsaved();
  return ok;
}

let suppress = 0;
if (typeof window !== 'undefined') {
  window.addEventListener('popstate', (e) => {
    if (suppress > 0) { suppress -= 1; e.stopImmediatePropagation(); return; }
    if (dirty.size === 0 || !ask) return;
    // Keep the router on this page: undo the step (the router never saw it), then ask.
    e.stopImmediatePropagation();
    suppress += 1;
    window.history.go(1);
    void ask().then((ok) => {
      if (!ok) return;
      discardUnsaved();
      window.history.go(-1);
    });
  });
}
