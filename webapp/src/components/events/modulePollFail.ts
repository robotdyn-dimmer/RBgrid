/**
 * When a module's current poll failure began, by the hub's own log (root 16.10, canon 10723b74):
 * `FAIL m<N> poll: … — module unreachable` on the first failed poll and once a minute while it
 * lasts, `m<N> poll restored after …` on recovery; a configured module silent at boot (canon 665ec474):
 * `FAIL m<N> (0x<addr>) not found at boot — …`, cleared by `m<N> (0x<addr>) joined late — …`.
 * `<N>` is the sensors list's `module`. Only these shapes are read — every other line stays opaque.
 */
import { HubEvent } from '../../types/hubEvents';

const FAIL = /^FAIL m(\d+)(?: \(0x[0-9a-fA-F]+\))? (poll\b|not found at boot)/;
const RESTORED = /^m(\d+)(?: \(0x[0-9a-fA-F]+\))? (?:poll restored|joined late)\b/;

export interface ModuleFail {
  /** unix s of the first FAIL line of the failure still open. */
  ts: number;
  /** 'boot' = not found at start-up (the hub keeps looking); 'poll' = stopped answering. */
  kind: 'poll' | 'boot';
}

/** module → the failure still open at the log's end. */
export function modulePollFailSince(events: HubEvent[]): Map<number, ModuleFail> {
  const open = new Map<number, ModuleFail>();
  for (const e of [...events].sort((a, b) => a.seq - b.seq)) {
    const f = FAIL.exec(e.msg);
    if (f) {
      const m = Number(f[1]);
      if (!open.has(m) && e.ts > 1_600_000_000) open.set(m, { ts: e.ts, kind: f[2] === 'poll' ? 'poll' : 'boot' });
      continue;
    }
    const r = RESTORED.exec(e.msg);
    if (r) open.delete(Number(r[1]));
  }
  return open;
}
