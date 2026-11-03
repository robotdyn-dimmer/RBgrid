/**
 * Release scope (v1.1.0, operator 20.10 via root): features whose code ships but whose UI is hidden
 * in a release build — ESP-NOW nodes, and the I2C bus tools (two buses / scan / address change /
 * per-module DRDY, `/api/v1/i2c`; no screens yet, the flag is for when they land), and the
 * split-phase mains system (operator 20.10: SPLIT deferred — not offered unless a hub already has it).
 *
 * Hidden in a production build; shown in dev and tests. `REACT_APP_PREVIEW_FEATURES=true|false`
 * overrides either way (e.g. a preview build for the bench).
 */
const override = process.env.REACT_APP_PREVIEW_FEATURES;
const preview = override === 'true' ? true : override === 'false' ? false : process.env.NODE_ENV !== 'production';

export const RELEASE_HIDDEN = {
  espnow: !preview,
  i2cBus: !preview,
  splitPhase: !preview,
  /** «Add a module» on Points (bus 0, v1.1.2 — operator 02.11 via root). Shown since root's .139 run
   *  (03.11); the page itself gates it on hub ≥ 1.1.2. */
  addModule: false,
  /** MQTT «Secure connection (TLS, mqtts)» (root 03.11): the hub takes mqtts:// but sets no CA / cert
   *  bundle, so the connection fails verification and was never tested. Hidden until root's HW run;
   *  a hub that already has an mqtts:// URI still shows it, read-only. */
  mqttTls: !preview,
  /** «Use as the three-phase input» on a U3I3 card (v1.1.2 rebind, operator 03.11 via root): shown since root's
   *  .139 gate (03.11, image 4456a483). */
  inputGroup: false,
  /** Settings → the hub's access point (operator 03.11 via root: into v1.1.2, hub ≥ 1.1.2 only):
   *  hidden until root's .139 run. */
  apPassword: false,
  /** Administration → «Factory reset» as a real reset (v1.1.2, operator 03.11 via root): hidden until
   *  root's .139 run. Older hubs get no reset button at all — their call only undid the last settings save. */
  factoryReset: false,
  /** «Via broker (MQTT)» on the connect screen of a hosted build (webapp.rbgrid.io): partial today —
   *  ~40 screens call the hub over HTTP directly, and token / AP / CORS / backup / OTA have no cmd path.
   *  Hidden until that's done; the hub kit never ships it (no MQTT chunk). */
  brokerTransport: !preview,
} as const;
