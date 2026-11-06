/**
 * Device time (UX audit W1-8: INP-11, RES-21, HOME-09). The hub keeps its own day — tariff zones
 * switch, daily buckets close and «сегодня» resets by ITS clock (time-config `timezone`, a POSIX
 * TZ string, e.g. "<UTC+9>-9", "CET-1CEST,M3.5.0,M10.5.0/3", "UTC0"). Hour axes, «сегодня»
 * windows and «смена зоны через …» are computed in that time, not the browser's.
 */
import { useEffect, useMemo } from 'react';
import { useConfigShadow } from './react/DeviceProvider';

interface Rule { month: number; week: number; day: number; timeSec: number }
export interface PosixTz { stdOffsetSec: number; dstOffsetSec?: number; start?: Rule; end?: Rule }

const OFF_RE = /^([+-]?)(\d{1,2})(?::(\d{1,2}))?(?::(\d{1,2}))?/;

function parseName(s: string): [string, string] | null {
  if (s.startsWith('<')) { const i = s.indexOf('>'); return i > 0 ? [s.slice(1, i), s.slice(i + 1)] : null; }
  const m = /^[A-Za-z]{3,}/.exec(s);
  return m ? [m[0], s.slice(m[0].length)] : null;
}
function parseOffset(s: string): [number, string] | null {
  const m = OFF_RE.exec(s);
  if (!m) return null;
  const sec = (Number(m[2]) * 3600 + Number(m[3] ?? 0) * 60 + Number(m[4] ?? 0)) * (m[1] === '-' ? -1 : 1);
  return [sec, s.slice(m[0].length)];
}
function parseRule(s: string): Rule | null {
  const m = /^M(\d{1,2})\.(\d)\.(\d)(?:\/([+-]?\d{1,3})(?::(\d{1,2}))?)?$/.exec(s);
  if (!m) return null;
  const h = m[4] != null ? Number(m[4]) : 2;
  return { month: Number(m[1]), week: Number(m[2]), day: Number(m[3]), timeSec: h * 3600 + Number(m[5] ?? 0) * 60 * Math.sign(h || 1) };
}

/** Parse a POSIX TZ string; null when it isn't one (then the browser's zone is used). */
export function parsePosixTz(tz: string | null | undefined): PosixTz | null {
  if (!tz || typeof tz !== 'string') return null;
  const n = parseName(tz.trim());
  if (!n) return null;
  const o = parseOffset(n[1]);
  if (!o) return /^(UTC|GMT|Z)$/i.test(n[0]) && !n[1] ? { stdOffsetSec: 0 } : null; // "<UTC+9>" without an offset — invalid (root R8 rejects it too)
  // POSIX offsets are WEST-positive: "CET-1" = UTC+1.
  const out: PosixTz = { stdOffsetSec: 0 - o[0] || 0 };
  let rest = o[1];
  if (!rest) return out;
  const dn = parseName(rest);
  if (!dn) return out;
  rest = dn[1];
  let dstOff = out.stdOffsetSec + 3600;
  const d = parseOffset(rest);
  if (d) { dstOff = -d[0]; rest = d[1]; }
  const rules = rest.startsWith(',') ? rest.slice(1).split(',') : [];
  const start = rules[0] ? parseRule(rules[0]) : null;
  const end = rules[1] ? parseRule(rules[1]) : null;
  if (start && end) { out.dstOffsetSec = dstOff; out.start = start; out.end = end; }
  return out;
}

/** UTC seconds of a Mm.w.d rule's moment in `year`, given the offset in force before it. */
function ruleUtc(year: number, r: Rule, offsetBeforeSec: number): number {
  const first = new Date(Date.UTC(year, r.month - 1, 1));
  let day = 1 + ((r.day - first.getUTCDay() + 7) % 7) + (r.week - 1) * 7;
  const dim = new Date(Date.UTC(year, r.month, 0)).getUTCDate();
  while (day > dim) day -= 7; // week 5 = last
  return Date.UTC(year, r.month - 1, day) / 1000 + r.timeSec - offsetBeforeSec;
}

/** Offset east of UTC (seconds) at UTC moment `tsSec`. */
export function tzOffsetAt(tz: PosixTz, tsSec: number): number {
  if (tz.dstOffsetSec == null || !tz.start || !tz.end) return tz.stdOffsetSec;
  const year = new Date(tsSec * 1000).getUTCFullYear();
  const s = ruleUtc(year, tz.start, tz.stdOffsetSec);
  const e = ruleUtc(year, tz.end, tz.dstOffsetSec);
  const inDst = s < e ? tsSec >= s && tsSec < e : tsSec >= s || tsSec < e;
  return inDst ? tz.dstOffsetSec : tz.stdOffsetSec;
}

export interface DeviceClock {
  /** The hub's zone is known (else the browser's local time is used). */
  known: boolean;
  /** Wall-clock parts of `tsSec` in the device's time. */
  parts: (tsSec: number) => { y: number; mo: number; d: number; h: number; mi: number };
  /** UTC seconds of the device-local midnight that starts the day containing `tsSec`. */
  dayStart: (tsSec: number) => number;
}

export function makeDeviceClock(tzString: string | null | undefined): DeviceClock {
  const tz = parsePosixTz(tzString);
  if (!tz) {
    return {
      known: false,
      parts: (ts) => { const d = new Date(ts * 1000); return { y: d.getFullYear(), mo: d.getMonth() + 1, d: d.getDate(), h: d.getHours(), mi: d.getMinutes() }; },
      dayStart: (ts) => { const d = new Date(ts * 1000); d.setHours(0, 0, 0, 0); return Math.floor(d.getTime() / 1000); },
    };
  }
  const parts = (ts: number) => {
    const d = new Date((ts + tzOffsetAt(tz, ts)) * 1000);
    return { y: d.getUTCFullYear(), mo: d.getUTCMonth() + 1, d: d.getUTCDate(), h: d.getUTCHours(), mi: d.getUTCMinutes() };
  };
  return {
    known: true,
    parts,
    dayStart: (ts) => {
      const off = tzOffsetAt(tz, ts);
      const local = ts + off;
      const localMidnight = local - (((local % 86400) + 86400) % 86400);
      // Midnight takes ITS OWN offset, not the moment's: on a changeover day the two differ
      // (Berlin 29.03 after 02:00 read 22:00Z, not 23:00Z). When local midnight doesn't exist
      // (Santiago's 24:00 spring-forward) this lands on the changeover, where the hub's day starts
      // too («T01:00:00-03:00», root tz-southern-presets).
      return localMidnight - tzOffsetAt(tz, localMidnight - off);
    },
  };
}

/** The connected hub's clock (time-config timezone), browser local time until it is known. */
export function useDeviceClock(): DeviceClock {
  const time = useConfigShadow('time');
  const rep = time.reported as Record<string, unknown>;
  const empty = !rep || Object.keys(rep).length === 0;
  // Pull time-config once if nobody has (the shadow keeps it for every later reader).
  // Concurrent readers share one GET (DeviceSession.loadDomain); a failed one is retried once after
  // 5 s, else the hub's day would silently fall back to the browser's clock (3φ audit H3P-03).
  useEffect(() => {
    if (!empty) return undefined;
    let timer: ReturnType<typeof setTimeout> | undefined;
    time.load().catch(() => { timer = setTimeout(() => { time.load().catch(() => undefined); }, 5000); });
    return () => { if (timer) clearTimeout(timer); };
  }, [empty]); // eslint-disable-line react-hooks/exhaustive-deps
  const cfg = (rep?.config as Record<string, unknown> | undefined) ?? rep;
  const tz = typeof cfg?.timezone === 'string' ? cfg.timezone : null;
  return useMemo(() => makeDeviceClock(tz), [tz]);
}

/** `Intl` formatting of `tsSec` as the HUB's wall clock (W1-8: History labels, table, tooltips):
 *  the hub's wall-clock parts are formatted as UTC, so the browser's own zone never leaks in. */
export function formatDeviceTime(clock: DeviceClock, tsSec: number, opts: Intl.DateTimeFormatOptions, locale?: string): string {
  const p = clock.parts(tsSec);
  // Zone offsets are whole minutes, so the seconds are the same everywhere (the hub log shows them).
  const sec = ((Math.floor(tsSec) % 60) + 60) % 60;
  return new Date(Date.UTC(p.y, p.mo - 1, p.d, p.h, p.mi, sec)).toLocaleString(locale, { ...opts, timeZone: 'UTC' });
}
