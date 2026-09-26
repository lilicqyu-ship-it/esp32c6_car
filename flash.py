#!/usr/bin/env python3
"""ESP32-C6 flashing helper - pure-Python twin of flash.bat (no PowerShell).

Runs under any Python 3.8+; the heavy lifting (esptool / parttool / packing)
is delegated to the ESP-IDF venv discovered from the EIM install metadata.

Usage:  python flash.py [mode] [COM port] [mon]
  mode   full (default) = bootloader + partition table + otadata + firmware
         assets         = control page only (repacks assets_src, firmware untouched)
         all            = assets first, then full firmware
  COM    default COM14 (e.g. python flash.py COM7)
  mon    open the serial monitor after flashing

Examples:
  python flash.py                # full flash to COM14
  python flash.py assets         # repack + flash control page only
  python flash.py all COM14 mon  # both, then monitor
"""
import json
import subprocess
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parent
BUILD = PROJECT / "build"
DEFAULT_PORT = "COM14"
BAUD = 460800
CHIP = "esp32c6"  # this helper is c6_car-specific

EIM_JSON = Path(r"C:\Espressif\tools\eim_idf.json")


def find_idf_env():
    """(venv python, IDF_PATH) from EIM metadata, env vars, then defaults."""
    if EIM_JSON.exists():
        try:
            inst = json.loads(EIM_JSON.read_text(encoding="utf-8"))["idfInstalled"][0]
            return Path(inst["python"]), Path(inst["path"])
        except (json.JSONDecodeError, KeyError, IndexError, TypeError):
            pass
    import os

    venv = os.environ.get("IDF_PYTHON_ENV_PATH")
    idf = os.environ.get("IDF_PATH")
    if venv and idf:
        return Path(venv) / "Scripts" / "python.exe", Path(idf)
    return (
        Path(r"C:\Espressif\tools\python\v6.1-beta1\venv\Scripts\python.exe"),
        Path(r"C:\esp\v6.1-beta1\esp-idf"),
    )


def run(cmd, **kw):
    print("[c6]", " ".join(str(c) for c in cmd))
    return subprocess.run([str(c) for c in cmd], **kw)


def flash_assets(py, idf, port):
    """Repack assets_src/ and write the assets partition (parttool)."""
    r = run([py, PROJECT / "tools" / "build_assets.py", "assets_src", BUILD / "assets.bin"],
            cwd=PROJECT)
    if r.returncode != 0:
        sys.exit("assets packing failed")
    parttool = idf / "components" / "partition_table" / "parttool.py"
    r = run([py, parttool, "-p", port, "write_partition",
             "--partition-name=assets", "--input", BUILD / "assets.bin"], cwd=BUILD)
    if r.returncode != 0:
        sys.exit("parttool write failed")


def flash_full(py, port):
    """Write the four flashing images exactly like `idf.py flash` does.

    build/flash_args carries the flash mode/freq/size line plus the
    offset/image pairs with build-relative paths, hence cwd=BUILD.
    """
    if not (BUILD / "c6_car.bin").exists():
        sys.exit("build/c6_car.bin missing - run `idf.py build` (or `flash.bat full`) first")
    run([py, "-m", "esptool", "--chip", CHIP, "-p", port, "-b", BAUD,
         "--before=default-reset", "--after=hard-reset",
         "write-flash", "@flash_args"], cwd=BUILD)


def monitor(port):
    """Serial monitor fallback when running outside the IDF venv (Ctrl+C to exit)."""
    try:
        import serial
    except ImportError:
        print("pyserial not available for monitor - run inside the IDF venv "
              "or use `idf.py -p %s monitor`" % port)
        return
    with serial.Serial(port, 115200, timeout=0.5) as ser:
        print(f"--- monitor {port} (Ctrl+C to exit) ---")
        try:
            while True:
                data = ser.read(4096)
                if data:
                    sys.stdout.write(data.decode("utf-8", errors="replace"))
                    sys.stdout.flush()
        except KeyboardInterrupt:
            pass


def parse_args(argv):
    mode, port, mon = "full", DEFAULT_PORT, False
    for a in argv:
        if a.lower() in ("full", "assets", "all"):
            mode = a.lower()
        elif a.lower() == "mon":
            mon = True
        elif a.lower().startswith("com") or a.startswith("/dev/"):
            port = a
        else:
            sys.exit(f"unknown argument: {a}\n{__doc__}")
    return mode, port, mon


def main():
    mode, port, mon = parse_args(sys.argv[1:])
    py, idf = find_idf_env()
    print(f"[c6] mode={mode} port={port}")
    print(f"[c6] idf={idf}")
    if not py.exists():
        sys.exit(f"IDF venv python not found: {py}")
    if mode in ("assets", "all"):
        flash_assets(py, idf, port)
    if mode in ("full", "all"):
        flash_full(py, port)
    if mon:
        monitor(port)


if __name__ == "__main__":
    main()
