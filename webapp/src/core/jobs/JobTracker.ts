/**
 * JobTracker — the "Jobs" primitive (AWS-IoT-modelled): async device ops tracked to
 * completion. Config-push, reboot, restore, OTA, ESP-NOW pairing all run as jobs so the
 * UI can show progress and reconnect-reconcile across a device reboot.
 *
 * Optimistic + observed: start() → running; resolve()/fail()/awaitReboot() move it on.
 * Pure (injectable clock + monotonic id counter — no Date/Math.random) so it's testable
 * and resume-safe.
 */

export type JobKind = 'config-push' | 'reboot' | 'restore' | 'ota' | 'espnow' | 'wifi-save';
export type JobStatus = 'running' | 'succeeded' | 'failed' | 'awaiting-reboot';

export interface Job {
  id: string;
  kind: JobKind;
  status: JobStatus;
  /** Free-form context, e.g. { domain: "tariff" } or { action: "pair" }. */
  detail?: Record<string, string>;
  error?: string;
  startedAt: number;
  endedAt?: number;
}

export class JobTracker {
  private _jobs: Job[] = [];
  private seq = 0;
  private readonly clock: () => number;
  private readonly onChange?: () => void;

  constructor(clock: () => number, onChange?: () => void) {
    this.clock = clock;
    this.onChange = onChange;
  }

  get jobs(): readonly Job[] {
    return this._jobs;
  }

  get active(): Job[] {
    return this._jobs.filter((j) => j.status === 'running' || j.status === 'awaiting-reboot');
  }

  start(kind: JobKind, detail?: Record<string, string>): Job {
    const job: Job = {
      id: `job-${++this.seq}`,
      kind,
      status: 'running',
      detail,
      startedAt: this.clock(),
    };
    this._jobs = [...this._jobs, job];
    this.onChange?.();
    return job;
  }

  private transition(id: string, status: JobStatus, error?: string): void {
    this._jobs = this._jobs.map((j) =>
      j.id === id
        ? { ...j, status, error, endedAt: status === 'awaiting-reboot' ? undefined : this.clock() }
        : j,
    );
    this.onChange?.();
  }

  resolve(id: string): void {
    this.transition(id, 'succeeded');
  }

  fail(id: string, error: string): void {
    this.transition(id, 'failed', error);
  }

  /** The op was accepted but the device is rebooting to converge; resolve on reconnect. */
  awaitReboot(id: string): void {
    this.transition(id, 'awaiting-reboot');
  }

  /** Drop finished jobs (keep running/awaiting-reboot). */
  clearFinished(): void {
    this._jobs = this._jobs.filter((j) => j.status === 'running' || j.status === 'awaiting-reboot');
    this.onChange?.();
  }
}
