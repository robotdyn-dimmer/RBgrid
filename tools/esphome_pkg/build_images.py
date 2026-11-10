#!/usr/bin/env python3
"""Build the ready-made RBgrid-for-ESPHome images from a generated package (dev tool; not shipped).

    build_images.py <package_dir> <out_dir> <version>

For each shipped config (esphome/rbgrid-hub-<board>.yaml):
  - compile it against the package's own sources (the git source is swapped for the local one, in a copy);
  - merge the factory image (one file, flashed at 0x0) from the build's flasher_args.json with the real offsets:
    ESPHome's own merge puts the app at 0x10000, where this partition table has otadata (app is at 0x20000);
  - copy the OTA image.
Writes rbgrid-esphome-<board>-v<version>.factory.bin / .ota.bin, manifest.json (ESP Web Tools) and SHA256SUMS.
"""
import hashlib, json, os, re, shutil, subprocess, sys

PKG, OUT, VER = (os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2]), sys.argv[3])
ESPHOME = os.path.expanduser("~/.local/bin/esphome")
BOARDS = {"esp32": "ESP32", "esp32s3": "ESP32-S3"}
GIT_SRC = re.compile(r"  - source:\n      type: git\n      url: https://github.com/rb-amp/rbgrid-esphome\n"
                     r"      ref: [^\n]+\n      path: esphome/components\n")


def esptool_python():
    return sys.executable   # the Python ESPHome runs in carries esptool


def build(board):
    src = f"{PKG}/esphome/rbgrid-hub-{board}.yaml"
    work = f"{PKG}/esphome/_build_{board}"
    os.makedirs(work, exist_ok=True)
    y = open(src, encoding="utf-8").read()
    y2 = GIT_SRC.sub("  - source:\n      type: local\n      path: ../components\n", y)
    if y2 == y:
        sys.exit(f"{src}: git source block not found")
    cfg = f"{work}/rbgrid-hub-{board}.yaml"
    open(cfg, "w", encoding="utf-8").write(y2)
    log = f"{work}/compile.log"
    for attempt in (1, 2):   # the first link after new files sometimes fails in this toolchain; a retry passes
        r = subprocess.run([ESPHOME, "compile", cfg], cwd=work, stdout=open(log, "w"), stderr=subprocess.STDOUT)
        if "Successfully compiled program" in open(log, errors="ignore").read():
            break
    else:
        sys.exit(f"{board}: compile failed, see {log}")
    name = "rbgrid-hub"   # esphome: name (substitution default); one build dir per board work dir
    env = f"{work}/.esphome/build/{name}/.pioenvs/{name}"
    fa = json.load(open(f"{env}/flasher_args.json"))
    # offsets by role from flasher_args.json; the files are where PlatformIO puts them (env root, its own names)
    files = {"bootloader": "bootloader.bin", "partition-table": "partitions.bin", "otadata": "ota_data_initial.bin",
             "app": "firmware.bin"}
    parts = []
    for role, fname in sorted(files.items(), key=lambda kv: int(fa[kv[0]]["offset"], 16)):
        path = os.path.join(env, fname)
        if not os.path.exists(path):
            sys.exit(f"{board}: {path} missing")
        parts += [fa[role]["offset"], path]
    chip = fa["extra_esptool_args"]["chip"]
    fs = fa["flash_settings"]
    factory = f"{OUT}/rbgrid-esphome-{board}-v{VER}.factory.bin"
    subprocess.run([esptool_python(), "-m", "esptool", "--chip", chip, "merge-bin", "-o", factory,
                    "--flash-mode", fs["flash_mode"], "--flash-freq", fs["flash_freq"], "--flash-size", fs["flash_size"]]
                   + parts, check=True)
    shutil.copy(f"{env}/firmware.ota.bin", f"{OUT}/rbgrid-esphome-{board}-v{VER}.ota.bin")
    return {"chipFamily": BOARDS[board], "parts": [{"path": os.path.basename(factory), "offset": 0}]}


def main():
    os.makedirs(OUT, exist_ok=True)
    builds = [build(b) for b in BOARDS]
    json.dump({"name": "RBgrid for ESPHome", "version": VER, "home_assistant_domain": "esphome",
               "new_install_prompt_erase": True, "builds": builds},
              open(f"{OUT}/manifest.json", "w"), indent=2)
    with open(f"{OUT}/SHA256SUMS", "w") as s:
        for f in sorted(os.listdir(OUT)):
            if f != "SHA256SUMS":
                s.write(hashlib.sha256(open(f"{OUT}/{f}", "rb").read()).hexdigest() + "  " + f + "\n")
    print(open(f"{OUT}/SHA256SUMS").read())


if __name__ == "__main__":
    main()
