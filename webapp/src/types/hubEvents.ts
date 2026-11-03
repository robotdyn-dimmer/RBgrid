/**
 * Hub event log — `GET /api/v1/system/events` (root canon 10723b74, DEVICE_API_SURFACE
 * «2026-10-17»). `msg` is a technical line for an engineer / support: shown as is, never parsed.
 */
export type HubEventLevel = 'periodic' | 'event' | 'fail';

export interface HubEvent {
  /** Running number across reboots. */
  seq: number;
  /** Boot number by the log. */
  b: number;
  /** Wall clock, unix s; 0 = never learnt (the line came before the clock was set). */
  ts: number;
  /** Uptime, s. */
  up: number;
  lvl: HubEventLevel;
  msg: string;
}

export interface HubEventsPage {
  events: HubEvent[];
  boot: number;
  seq_last: number;
  ram_first?: number;
  next: number;
  more: boolean;
  /** false = the log files are unavailable (only this boot's RAM ring). */
  persist: boolean;
  dropped?: number;
  /** FAIL lines since the current boot. */
  fail_boot: number;
  up?: number;
  now?: number;
}

/** `GET /api/system/version`. `image_md5` first 8 hex = the firmware canon id. */
export interface HubVersion {
  version?: string;
  project?: string;
  idf?: string;
  build_date?: string;
  partition?: string;
  image_md5?: string;
}
