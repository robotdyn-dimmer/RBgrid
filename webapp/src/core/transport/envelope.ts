/**
 * Config GET envelopes — the device answers a config read in three shapes (verified on hw
 * 2026-06-29), over HTTP and, verbatim, over the MQTT cmd channel. One parser for both transports.
 */
import { JsonObject, JsonValue } from '../json';

const ENVELOPE_META = new Set(['type', 'version', 'device_id', 'timestamp_local', 'timestamp_ntp']);

/** Normalize a config GET body to its inner config object across the device's 3 shapes. */
export function extractConfigPayload(body: unknown): JsonObject {
  if (!body || typeof body !== 'object') return {};
  const obj = body as Record<string, unknown>;
  const keys = Object.keys(obj).filter((k) => !ENVELOPE_META.has(k));
  // 1) {type,…,data:{…}} — the common base-wrapper. Sibling keys next to `data` are merged in
  //    without overriding it (telemetry-config puts its writable `profile` at the ROOT).
  if (obj.data && typeof obj.data === 'object' && !Array.isArray(obj.data)) {
    const inner = { ...(obj.data as JsonObject) };
    for (const k of keys) if (k !== 'data' && !(k in inner)) inner[k] = obj[k] as JsonValue;
    return inner;
  }
  // 2) {<section>:{…}} — a single nested object (e.g. system-config → {system:{…}})
  if (keys.length === 1) {
    const only = obj[keys[0]];
    if (only && typeof only === 'object' && !Array.isArray(only)) return only as JsonObject;
  }
  // 3) bare object (e.g. mqtt-config {broker_uri,…}) — strip envelope meta, return the rest
  const rest: JsonObject = {};
  for (const k of keys) rest[k] = obj[k] as JsonValue;
  return rest;
}
