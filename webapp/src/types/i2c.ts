/**
 * I2C fleet API (`main/rbi2c_api.c`, in the firmware since v1.1.0). Only bus 0 and the add /
 * readdress path are used by the app (operator 02.11 via root: «up to 3 modules» in v1.1.2);
 * the second bus, pins and per-module DRDY stay out of the UI.
 */

/** GET /api/v1/i2c → modules[]: one configured rbAmp module. */
export interface I2cModule {
  /** Fleet index (the `module` of /api/v1/sensors). */
  module: number;
  bus: number;
  /** 7-bit address, e.g. 80 = 0x50. */
  addr: number;
  present: boolean;
  model?: string;
  n_i?: number;
  /** Channels bound to a point. */
  points?: number;
}

export interface I2cBus { bus: number; up: boolean; configured?: boolean; sda?: number; scl?: number }

/** GET /api/v1/i2c (and the body of a successful write, with `status:"ok"`). */
export interface I2cState {
  buses: I2cBus[];
  modules: I2cModule[];
  max_modules?: number;
  reboot_pending?: boolean;
  status?: string;
}

/** POST /api/v1/i2c/scan → devices[]: one address that answered. */
export interface I2cDevice {
  addr: number;
  /** Fleet index when the hub already has a module there, else null. */
  module: number | null;
  /** rbamp = an rbAmp module; rbdimmer / unknown = something else at 0x50–0x5F; device = outside that range. */
  kind: 'rbamp' | 'rbdimmer' | 'unknown' | 'device';
  fw?: string;
  uid?: string;
  /** v1.1.2 scan: product line «basic» / «std» and model, e.g. «BASIC-UI3», «STD-U3I3». */
  line?: string;
  model?: string;
}

export type I2cScanResult =
  | { status: 'ok'; devices: I2cDevice[] }
  | { status: 'error'; code: string; message: string; http?: number };

export type I2cWriteResult =
  | ({ status: 'ok' } & I2cState)
  | { status: 'error'; code: string; message: string; http?: number };
