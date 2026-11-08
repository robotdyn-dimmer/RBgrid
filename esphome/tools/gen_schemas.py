# SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Infer the per-reply JSON Schemas from fetched samples: gen_schemas.py <samples dir> <schemas dir>."""
import json, sys, os
SRC, DST = sys.argv[1], sys.argv[2]
def t(v):
    if isinstance(v, bool): return "boolean"
    if isinstance(v, int): return "integer"
    if isinstance(v, float): return "number"
    if isinstance(v, str): return "string"
    if v is None: return "null"
    if isinstance(v, list): return "array"
    return "object"
def merge_types(a, b):
    s = set(a if isinstance(a, list) else [a]) | set(b if isinstance(b, list) else [b])
    if "integer" in s and "number" in s: s.discard("integer")
    s = sorted(s); return s[0] if len(s) == 1 else s
def infer(v):
    ty = t(v)
    if ty == "object":
        props = {k: infer(x) for k, x in v.items()}
        return {"type": "object", "required": sorted(v.keys()), "properties": props}
    if ty == "array":
        sc = None
        for x in v: sc = infer(x) if sc is None else merge(sc, infer(x))
        return {"type": "array", "items": sc} if sc else {"type": "array"}
    if ty in ("integer", "number"): return {"type": "number"}   # the device prints floats without a fraction too
    return {"type": ty}
def merge(a, b):
    if a.get("type") == "object" and b.get("type") == "object":
        props = dict(a["properties"])
        for k, s in b["properties"].items(): props[k] = merge(props[k], s) if k in props else s
        return {"type": "object", "required": sorted(set(a["required"]) & set(b["required"])), "properties": props}
    if a.get("type") == "array" and b.get("type") == "array":
        if "items" in a and "items" in b: return {"type": "array", "items": merge(a["items"], b["items"])}
        return a if "items" in a else b
    if a == b: return a
    return {"type": merge_types(a.get("type"), b.get("type"))}
DESC = {
 "schema": ("get schema", "Enums, capabilities and quality bits (= GET /api/v1/schema)."),
 "sensors": ("get sensors", "Modules, channels, CT models, bound points (= GET /api/v1/sensors)."),
 "units": ("get units", "Every point with its settings (= GET /api/system/units-config)."),
 "telemetry_data": ("get telemetry_data", "Live frame: system, tariff now, points (= GET /api/telemetry/latest)."),
 "tariff": ("get tariff", "The tariff plan (= GET /api/system/tariff-plan-config)."),
 "system": ("get system", "System config (= GET /api/system/system-config)."),
 "economics": ("get economics", "Economics of one point (= GET /api/v1/economics)."),
 "summary": ("get summary", "Energy/money summary of one point over a range (= GET /api/v1/summary)."),
 "i2c": ("get i2c", "I2C buses, board pin profile, modules (= GET /api/v1/i2c)."),
 "espnow": ("get espnow", "ESP-NOW nodes and pairing state (= GET /api/v1/espnow)."),
 "price": ("get price", "External prices stored on the device."),
 "history": ("history", "Stored history rows (= GET /api/v1/history)."),
 "series": ("series", "Energy and money per stored period, cols [ts,dur,wh_in,wh_out,cost,revenue,q]; ts = END of interval."),
 "ack_err": ("cmd_ack", "Acknowledgement of a changing command (set/unit); data.result ok|error."),
 "scan": ("set i2c scan", "Reply of an I2C action (data.reply carries the action result)."),
}
os.makedirs(DST, exist_ok=True)
for name, (cmd, d) in DESC.items():
    smp = json.load(open(os.path.join(SRC, name + ".json")))["data"]
    out_name = "cmd_ack" if name == "ack_err" else ("i2c_action" if name == "scan" else name)
    sc = infer(smp)
    doc = {"$schema": "https://json-schema.org/draft/2020-12/schema",
           "$id": f"rbgrid_cmd/{out_name}/1", "title": f"rbgrid_cmd reply: {cmd}", "description":
           d + " Schema 1 (reply field `schema`). Inferred from a live device (ESPHome build 1.2.0-dev, 2026-11-08): "
           "every listed key is present in that reply; extra keys may appear and must be ignored.", **sc}
    if name == "ack_err":   # the sample is an error ack: result/message are the general shape
        pass
    json.dump(doc, open(os.path.join(DST, f"{out_name}.schema.json"), "w"), indent=1, ensure_ascii=False)
    print(out_name, len(json.dumps(doc)))
