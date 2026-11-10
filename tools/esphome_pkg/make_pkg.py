#!/usr/bin/env python3
"""Build the clean RBgrid-for-ESPHome package from the dev tree (dev tool; not shipped).

What goes in:
  - the C/C++ files the three ESPHome builds actually compile, plus the headers they include (from the builds'
    compile_commands.json + a quoted-include closure); run `esphome compile` for all example YAMLs first;
  - with the blocks for interfaces the ESPHome build doesn't have cut out (web server, MQTT, Wi-Fi manager, mDNS,
    SNTP, bench console, fault injection): a small preprocessor folds every #if that depends only on those switches
    (and on the macros the files derive from them); an #if with any other symbol is kept as it is;
  - component CMake/Kconfig/manifests, the ESPHome component, docs, schemas, example YAMLs, partitions, licence.
What stays out: everything else (web server/WebSocket/MQTT/Wi-Fi sources, the web server's development certificate
and key, wifi_secrets.h, key files, bench components, the standalone app, build trees).

Usage: make_pkg.py <out_dir>        (out_dir must not exist)
"""
import json, os, re, shutil, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.abspath(sys.argv[1])
YAMLS = ("rbgrid-hub-c14", "rbgrid-hub-s3", "rbgrid-hub-3p")

# Switches that are off in every ESPHome build (the component sets the CONFIG_ ones; the others are never defined).
OFF = {"CONFIG_RBGRID_IF_HTTP", "CONFIG_RBGRID_IF_MQTT", "CONFIG_RBGRID_IF_WIFI", "CONFIG_RBGRID_IF_MDNS",
       "CONFIG_RBGRID_IF_SNTP", "CONFIG_RBGRID_DEBUG_BRIDGE", "CONFIG_RBGRID_BENCH_FULL", "CONFIG_RBGRID_BENCH_KIT",
       "RBGRID_FAULT_INJECT", "CONFIG_RBGRID_PANIC_FORENSICS", "RBGRID_BENCH_KIT"}
NEVER = ("wifi_secrets.h", "espnow_keys.h", "ap_provision.h")
MIT = """MIT License

Copyright (c) 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
"""
NOTICE = """RBgrid for ESPHome
Copyright (c) 2025-2026 Zhuhai RobotDyn Technology Co., Ltd. Licensed under the MIT License (see LICENSE).

This package includes or uses third-party software under its own license; those terms apply to those parts.

1. components/esp_driver_i2c - modified copies of i2c_master.c from the ESP-IDF v5.4.1 and v5.5.1 I2C driver
   (the other driver files are used from ESP-IDF unchanged), Copyright (c) Espressif Systems (Shanghai) CO LTD.
   License: Apache License 2.0 (http://www.apache.org/licenses/LICENSE-2.0).
   Modifications: bounded post-NACK bus wait, ISR guard on non-READ operations, timeout status ordering,
   interrupt masking on NACK/timeout in synchronous mode - each marked "RBGRID PATCH" in i2c_master.c.

2. ESP-IDF framework and ESPHome (build dependencies, not shipped here). ESP-IDF: Apache License 2.0,
   Copyright (c) Espressif Systems; includes cJSON (MIT, Copyright (c) 2009-2017 Dave Gamble and cJSON contributors).

3. espressif/mdns (managed component) - Apache License 2.0, Copyright (c) Espressif Systems.

4. joltwallet/littlefs (managed component) - MIT License, Copyright (c) 2020 Brian Pugh;
   bundles littlefs - BSD 3-Clause, Copyright (c) 2022, The littlefs authors; Copyright (c) 2017, Arm Limited.

5. rbamp (rbamp-esp-idf metering module client, managed component) - MIT License,
   Copyright (c) 2025 rbAmp Contributors.
"""
FORBIDDEN_RE = re.compile(r"(key\.pem$|wifi_secrets\.h$|espnow_keys\.h$|ap_provision\.h$)")


def used_files():
    src = set()
    for y in YAMLS:
        cc = f"{ROOT}/esphome/.esphome/build/{y}/.pioenvs/{y}/compile_commands.json"
        if not os.path.exists(cc):
            sys.exit(f"missing {cc}: run `esphome compile esphome/{y}.yaml` first")
        for e in json.load(open(cc)):
            f = os.path.normpath(os.path.join(e.get("directory", ""), e["file"]))
            if f.startswith(ROOT + "/components/"):
                src.add(f)
    byname = {}
    for d, _, fs in os.walk(ROOT + "/components"):
        for f in fs:
            byname.setdefault(f, []).append(os.path.join(d, f))
    seen, stack = set(), list(src)
    while stack:
        f = stack.pop()
        if f in seen:
            continue
        seen.add(f)
        txt = open(f, encoding="utf-8", errors="ignore").read()
        if f.endswith((".c", ".cpp", ".h")):
            txt = strip(txt)   # includes inside cut blocks don't count
        for inc in re.findall(r'#\s*include\s+"([^"]+)"', txt):
            if os.path.basename(inc) in NEVER:
                continue   # optional local secrets (__has_include): never part of the package
            for cand in byname.get(os.path.basename(inc), []):
                if cand.endswith(inc.replace("\\", "/")):
                    stack.append(cand)
    return seen


# ---- mini preprocessor -------------------------------------------------------------------------------------------
TOK = re.compile(r"\s*(defined\s*\(\s*\w+\s*\)|defined\s+\w+|\w+|&&|\|\||!|\(|\)|==|!=|<=|>=|<|>|\S)")


def evaluate(expr, defs):
    """defs: name -> int (known value) or None (known undefined). Returns 0/1, or None if any other symbol is used."""
    out = []
    for t in TOK.findall(expr.split("//")[0].split("/*")[0]):
        m = re.match(r"defined\s*\(?\s*(\w+)", t)
        if m:
            n = m.group(1)
            if n not in defs:
                return None
            out.append("1" if defs[n] is not None else "0")
        elif re.fullmatch(r"\d+[uUlL]*", t):
            out.append(t.rstrip("uUlL"))
        elif re.fullmatch(r"\w+", t):
            if t not in defs:
                return None
            out.append(str(defs[t] or 0))
        else:
            out.append({"&&": " and ", "||": " or ", "!": " not "}.get(t, t))
    try:
        return 1 if eval("".join(out), {"__builtins__": {}}) else 0
    except Exception:
        return None


def strip(text):
    defs = {n: None for n in OFF}
    lines, out = text.split("\n"), []
    stack = []   # frames: {"keep": bool (unknown frame kept verbatim), "active": bool, "taken": bool, "parent": bool}
    def active():
        return all(f["active"] for f in stack)
    i = 0
    while i < len(lines):
        line = lines[i]
        full = line
        while full.endswith("\\") and i + 1 < len(lines):   # continued directive
            i += 1; full = full[:-1] + " " + lines[i]; line = line + "\n" + lines[i]
        m = re.match(r"\s*#\s*(ifdef|ifndef|if|elif|else|endif|define|undef)\b(.*)", full)
        if not m:
            if active():
                out.append(line)
            i += 1
            continue
        d, rest = m.group(1), m.group(2).strip()
        par = active()
        if d in ("if", "ifdef", "ifndef"):
            if d == "if":
                v = evaluate(rest, defs)
            else:
                n = rest.split()[0]
                v = None if n not in defs else int((defs[n] is not None) == (d == "ifdef"))
            if v is None:
                stack.append({"keep": True, "active": True})
                if par: out.append(line)
            else:
                stack.append({"keep": False, "active": bool(v), "taken": bool(v)})
        elif d == "elif":
            f = stack[-1]
            if f["keep"]:
                if par or all(x["active"] for x in stack[:-1]): out.append(line)
            else:
                v = evaluate(rest, defs)
                if v is None:
                    raise SystemExit(f"undecidable #elif after a decided #if: {full.strip()}")
                f["active"] = (not f["taken"]) and bool(v)
                f["taken"] = f["taken"] or bool(v)
        elif d == "else":
            f = stack[-1]
            if f["keep"]:
                if all(x["active"] for x in stack[:-1]): out.append(line)
            else:
                f["active"] = not f["taken"]; f["taken"] = True
        elif d == "endif":
            f = stack.pop()
            if f["keep"] and active():
                out.append(line)
        else:   # define / undef
            if par:
                out.append(line)
                parts = rest.split(None, 1)
                if parts:
                    n = parts[0]
                    if d == "undef":
                        defs.pop(n, None)
                    elif "(" not in n:
                        val = parts[1].split("//")[0].split("/*")[0].strip() if len(parts) > 1 else "1"
                        if re.fullmatch(r"\d+", val):
                            defs[n] = int(val)
                        elif n in defs:
                            defs.pop(n)   # redefined to something not literal: unknown from here on
        i += 1
    assert not stack, "unbalanced #if"
    return "\n".join(out)


def copy_code(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    txt = open(src, encoding="utf-8").read()
    open(dst, "w", encoding="utf-8", newline="").write(strip(txt) if src.endswith((".c", ".cpp", ".h")) else txt)


def cmake_strip(path, drop_blocks):
    """Drop `if(<cond>) … endif()` blocks whose condition names one of drop_blocks (top-level, non-nested)."""
    s = open(path, encoding="utf-8").read()
    for cond in drop_blocks:
        s = re.sub(r"\nif\(" + re.escape(cond) + r"\)\n(?:.*\n)*?endif\(\)\n", "\n", s)
    return s


def main():
    if os.path.exists(OUT):
        sys.exit(f"{OUT} exists")
    files = used_files()
    for f in sorted(files):
        copy_code(f, OUT + f[len(ROOT):])
    comp = ROOT + "/components"
    # component build files
    for c in ("rbpower", "rbgrid_app", "esp_driver_i2c"):
        for n in ("CMakeLists.txt", "Kconfig", "Kconfig.projbuild", "idf_component.yml", "README.md"):
            p = f"{comp}/{c}/{n}"
            if os.path.exists(p):
                os.makedirs(f"{OUT}/components/{c}", exist_ok=True)
                shutil.copy(p, f"{OUT}/components/{c}/{n}")
    # the patched I2C driver: copy both IDF-minor variants
    shutil.copytree(f"{comp}/esp_driver_i2c", f"{OUT}/components/esp_driver_i2c", dirs_exist_ok=True)
    # lfs_config.h etc. that are found through include dirs rather than quoted includes
    for d in ("rbpower/include",):
        for n in os.listdir(f"{comp}/{d}"):
            p = f"{OUT}/components/{d}/{n}"
            if not os.path.exists(p) and n.startswith("lfs_"):
                copy_code(f"{comp}/{d}/{n}", p)
    # rbpower CMake: no interface sources, no certificate (never copied)
    rb = f"{OUT}/components/rbpower/CMakeLists.txt"
    s = cmake_strip(rb, ["CONFIG_RBGRID_IF_WIFI", "CONFIG_RBGRID_IF_HTTP", "CONFIG_RBGRID_IF_MQTT",
                         "CONFIG_RBGRID_IF_HTTP OR CONFIG_RBGRID_IF_MQTT", "RBGRID_FAULT_INJECT"])
    open(rb, "w", encoding="utf-8", newline="").write(s)
    ra = f"{OUT}/components/rbgrid_app/CMakeLists.txt"
    s = cmake_strip(ra, ["CONFIG_RBGRID_IF_HTTP", "CONFIG_RBGRID_BENCH_KIT", "CONFIG_RBGRID_PANIC_FORENSICS"])
    s = "\n".join(l for l in s.split("\n") if not (l.startswith("#") and re.search(r"BENCH_KIT|[Pp]anic forensics|coredump", l)))
    s = re.sub(r"\n{3,}", "\n\n", s).strip() + "\n"
    open(ra, "w", encoding="utf-8", newline="").write(s)
    # bench stubs (header-only, empty) stay only if a kept file still includes them
    for stub in ("rbbench", "bench_kit"):
        needed = any(re.search(r'#\s*include\s+"' + stub + r'\.h"', open(p, encoding="utf-8", errors="ignore").read())
                     for p in (os.path.join(d, f) for d, _, fs in os.walk(OUT) for f in fs if f.endswith((".c", ".cpp", ".h"))))
        if needed:
            shutil.copytree(f"{comp}/{stub}", f"{OUT}/components/{stub}", dirs_exist_ok=True)
        else:   # not shipped: drop it from rbgrid_app's REQUIRES and the comment that explains it
            s = open(ra, encoding="utf-8").read()
            s = re.sub(r"\n\s*" + stub + r"\s*(#[^\n]*)?(?=\n)", "", s)
            s = "\n".join(l for l in s.split("\n") if not (l.startswith("#") and stub in l))
            open(ra, "w", encoding="utf-8", newline="").write(s)
    # ESPHome side
    e = ROOT + "/esphome"
    shutil.copytree(f"{e}/components/rbgrid", f"{OUT}/esphome/components/rbgrid", ignore=shutil.ignore_patterns("__pycache__"))
    shutil.copytree(f"{e}/schemas", f"{OUT}/esphome/schemas")
    for n in ("README.md", "RBGRID_CMD.md", "partitions_rbgrid_esphome.csv") + tuple(y + ".yaml" for y in YAMLS):
        shutil.copy(f"{e}/{n}", f"{OUT}/esphome/{n}")
    open(f"{OUT}/esphome/secrets.example.yaml", "w").write(
        "# copy to secrets.yaml and fill in\nwifi_ssid: \"your-network\"\nwifi_password: \"your-password\"\n")
    # licence of the rbgrid-esphome repository: MIT (operator 2026-11-10)
    open(f"{OUT}/LICENSE", "w", encoding="utf-8").write(MIT)
    open(f"{OUT}/NOTICE", "w", encoding="utf-8").write(NOTICE)
    text_ext = (".c", ".cpp", ".h", ".py", ".md", ".yaml", ".yml", ".txt", ".csv", ".json", "CMakeLists.txt", "Kconfig",
                "Kconfig.projbuild")
    for d, _, fs in os.walk(OUT):
        for f in fs:
            p = os.path.join(d, f)
            if not f.endswith(text_ext):
                continue
            s = open(p, encoding="utf-8", errors="ignore").read()
            s2 = re.sub(r"SPDX-License-Identifier:\s*PolyForm-Noncommercial-1\.0\.0", "SPDX-License-Identifier: MIT", s)
            s2 = s2.replace("Licence: PolyForm Noncommercial 1.0.0", "Licence: MIT").replace(
                "license: PolyForm-Noncommercial-1.0.0", "license: MIT")
            s2 = s2.replace("`<repo>`", "`rb-amp/rbgrid-esphome`").replace("<repo>", "rb-amp/rbgrid-esphome").replace(
                "<org>/rbgrid-ha", "rb-amp/rbgrid-ha")
            if s2 != s:
                open(p, "w", encoding="utf-8", newline="").write(s2)
    left = [os.path.join(d, f)[len(OUT) + 1:] for d, _, fs in os.walk(OUT) for f in fs
            if f.endswith(text_ext) and "PolyForm" in open(os.path.join(d, f), encoding="utf-8", errors="ignore").read()]
    if left:
        sys.exit(f"licence text left to fix: {left}")
    # guards
    bad = [os.path.join(d, f) for d, _, fs in os.walk(OUT) for f in fs if FORBIDDEN_RE.search(f)]
    if bad:
        sys.exit(f"forbidden files in the package: {bad}")
    for d, _, fs in os.walk(OUT):
        for f in fs:
            if f.endswith((".c", ".cpp", ".h")) and re.search(r"BEGIN [A-Z ]*PRIVATE KEY-----\n", open(os.path.join(d, f), errors="ignore").read()):
                sys.exit(f"a private key block in {f}")
    n = sum(len(fs) for _, _, fs in os.walk(OUT))
    print(f"package: {OUT} ({n} files)")


if __name__ == "__main__":
    main()
