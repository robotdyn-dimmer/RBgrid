/**
 * JSON primitives + the delta engine that powers the config shadow.
 *
 * The delta follows AWS IoT Device Shadow semantics: the delta is the set of
 * fields in `desired` that differ from `reported`. Fields present only in
 * `reported` (not in `desired`) are NOT part of the delta — desired is the
 * intent, reported is the truth, and we only ever push the diff toward the device.
 * Arrays are replaced wholesale on any mismatch (AWS copies the whole desired
 * array into the delta); nested objects are recursed.
 */

export type JsonPrimitive = string | number | boolean | null;
export type JsonValue = JsonPrimitive | JsonValue[] | { [key: string]: JsonValue };
export type JsonObject = { [key: string]: JsonValue };

/** Structural deep-equality for JSON values (order-sensitive for arrays). */
export function deepEqual(a: JsonValue, b: JsonValue): boolean {
  if (a === b) return true;
  if (a === null || b === null) return a === b;
  if (typeof a !== typeof b) return false;

  if (Array.isArray(a) || Array.isArray(b)) {
    if (!Array.isArray(a) || !Array.isArray(b) || a.length !== b.length) return false;
    return a.every((v, i) => deepEqual(v, b[i]));
  }

  if (typeof a === 'object' && typeof b === 'object') {
    const ak = Object.keys(a);
    const bk = Object.keys(b);
    if (ak.length !== bk.length) return false;
    return ak.every((k) => k in b && deepEqual(a[k], (b as JsonObject)[k]));
  }

  return false;
}

/**
 * The AWS-IoT-style delta from `reported` → `desired`: only the keys whose
 * desired value differs from reported. Returns `{}` when they fully agree.
 * Nested objects recurse (so an unchanged sibling key isn't dragged in);
 * arrays/primitives replace wholesale.
 */
export function computeDelta(reported: JsonObject, desired: JsonObject): JsonObject {
  const delta: JsonObject = {};
  for (const key of Object.keys(desired)) {
    const d = desired[key];
    const r = reported[key];
    if (deepEqual(r as JsonValue, d)) continue;

    if (
      d !== null && typeof d === 'object' && !Array.isArray(d) &&
      r !== null && typeof r === 'object' && !Array.isArray(r)
    ) {
      const sub = computeDelta(r as JsonObject, d as JsonObject);
      if (Object.keys(sub).length > 0) delta[key] = sub;
    } else {
      delta[key] = d;
    }
  }
  return delta;
}

/** All dotted leaf-paths that differ (for reboot-impact checks + diff display). */
export function deltaPaths(delta: JsonObject, prefix = ''): string[] {
  const paths: string[] = [];
  for (const key of Object.keys(delta)) {
    const v = delta[key];
    const path = prefix ? `${prefix}.${key}` : key;
    if (v !== null && typeof v === 'object' && !Array.isArray(v)) {
      paths.push(...deltaPaths(v as JsonObject, path));
    } else {
      paths.push(path);
    }
  }
  return paths;
}

/** Deep clone of a JSON object (structuredClone-free for older runtimes). */
export function cloneJson<T extends JsonValue>(v: T): T {
  return JSON.parse(JSON.stringify(v)) as T;
}
