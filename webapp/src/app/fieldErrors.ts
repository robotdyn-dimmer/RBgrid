/**
 * Inline field errors across the Settings page (UX audit W1-7): the global «Применить» stays off
 * while any field shows an error, and «Сбросить» clears them (bumps `epoch` → the fields remount
 * with the device value).
 */
import { useSyncExternalStore } from 'react';

const errors = new Map<string, string>();
let epoch = 0;
const subs = new Set<() => void>();
const emit = () => { subs.forEach((f) => f()); };
const subscribe = (f: () => void) => { subs.add(f); return () => { subs.delete(f); }; };

export function setFieldError(id: string, err: string | null): void {
  if (err) { if (errors.get(id) === err) return; errors.set(id, err); } else if (!errors.delete(id)) return;
  emit();
}
export function clearFieldErrors(prefix?: string): void {
  let changed = false;
  errors.forEach((_, k) => { if (!prefix || k.startsWith(prefix)) { errors.delete(k); changed = true; } });
  if (!prefix) { epoch += 1; changed = true; }
  if (changed) emit();
}
export const useFieldErrorCount = (): number => useSyncExternalStore(subscribe, () => errors.size);
export const useFieldEpoch = (): number => useSyncExternalStore(subscribe, () => epoch);
