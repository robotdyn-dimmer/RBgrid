# SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Fetch full rbgrid_cmd replies (parts reassembled) from a device: fetch_samples.py <outdir> name=<json cmd> ... (host 192.168.0.157)."""
import asyncio, json, sys, base64, os
from aioesphomeapi import APIClient
OUT = sys.argv[1]; CMDS = [(a.split('=',1)[0], json.loads(a.split('=',1)[1])) for a in sys.argv[2:]]
async def call(c, svc, cmd):
    r = await c.execute_service(svc, {"command": json.dumps(cmd)}, return_response=True)
    return r, json.loads(r.response_data.decode()) if r.response_data else None
async def main():
    c = APIClient("192.168.0.157", 6053, None); await c.connect(login=True)
    _, svcs = await c.list_entities_services(); svc = [s for s in svcs if s.name == "rbgrid_cmd"][0]
    for name, cmd in CMDS:
        r, w = await call(c, svc, cmd)
        if w and "rid" in w:
            buf = base64.b64decode(w["b64"]); 
            for i in range(1, w["parts"]):
                _, p = await call(c, svc, {"cmd":"part","rid":w["rid"],"part":i}); buf += base64.b64decode(p["b64"])
            data = json.loads(buf); wrap = {k:v for k,v in w.items() if k not in ("b64",)}
        else:
            data = w.get("data") if w else None; wrap = {k:v for k,v in (w or {}).items() if k != "data"}
        json.dump({"wrapper": wrap, "data": data}, open(os.path.join(OUT, name + ".json"), "w"), indent=1)
        print(name, r.success, len(json.dumps(data)))
    await c.disconnect()
asyncio.run(main())
