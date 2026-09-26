#!/usr/bin/env python3
"""ESP32-C6 flashing helper - click CLI, pure-Python twin of flash.bat.

Commands:
  full    bootloader + partition table + otadata + firmware (default)
  assets  control page only (repacks assets_src, firmware untouched)
  all     assets first, then full firmware
  mon     serial monitor only (Ctrl+C to exit)

Examples:
  python flash.py                 # same as: full
  python flash.py all -p COM7 -m  # both images, port COM7, monitor after
  python flash.py assets          # repack + flash control page only

Runs under any Python 3.8+; if `click` is missing the script relaunches
itself under the ESP-IDF venv interpreter (discovered from EIM metadata),
which always has click (idf.py depends on it).
"""
import json
import os
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
    venv = os.environ.get("IDF_PYTHON_ENV_PATH")
    idf = os.environ.get("IDF_PATH")
    if venv and idf:
        return Path(venv) / "Scripts" / "python.exe", Path(idf)
    return (
        Path(r"C:\Espressif\tools\python\v6.1-beta1\venv\Scripts\python.exe"),
        Path(r"C:\esp\v6.1-beta1\esp-idf"),
    )


try:
    import click
except ImportError:
    if os.environ.get("C6_FLASH_REEXEC") != "1":
        py, _ = find_idf_env()
        if py.exists():
            print(f"[c6] click missing - relaunching under IDF venv: {py}")
            env = dict(os.environ, C6_FLASH_REEXEC="1")
            os.execve(str(py), [str(py), __file__, *sys.argv[1:]], env)
    sys.exit("click is required: pip install click (or use the IDF venv python)")


def run(cmd, **kw):
    print("[c6]", " ".join(str(c) for c in cmd))
    return subprocess.run([str(c) for c in cmd], **kw)


def flash_assets(port):
    """Repack assets_src/ and write the assets partition (parttool)."""
    py, idf = find_idf_env()
    r = run([py, PROJECT / "tools" / "build_assets.py", "assets_src", BUILD / "assets.bin"],
            cwd=PROJECT)
    if r.returncode != 0:
        sys.exit("assets packing failed")
    parttool = idf / "components" / "partition_table" / "parttool.py"
    r = run([py, parttool, "-p", port, "write_partition",
             "--partition-name=assets", "--input", BUILD / "assets.bin"], cwd=BUILD)
    if r.returncode != 0:
        sys.exit("parttool write failed")


def flash_full(port):
    """Write the four flashing images exactly like `idf.py flash` does.

    build/flash_args carries the flash mode/freq/size line plus the
    offset/image pairs with build-relative paths, hence cwd=BUILD.
    """
    py, _ = find_idf_env()
    if not (BUILD / "c6_car.bin").exists():
        sys.exit("build/c6_car.bin missing - run `idf.py build` (or `flash.bat full`) first")
    run([py, "-m", "esptool", "--chip", CHIP, "-p", port, "-b", BAUD,
         "--before=default-reset", "--after=hard-reset",
         "write-flash", "@flash_args"], cwd=BUILD)


def do_monitor(port):
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


def common_opts(f):
    f = click.option("-p", "--port", default=DEFAULT_PORT, show_default=True,
                     help="Serial port.")(f)
    f = click.option("-m", "--monitor", is_flag=True,
                     help="Open the serial monitor after flashing.")(f)
    return f


@click.group(invoke_without_command=True,
             context_settings={"help_option_names": ["-h", "--help"]})
@click.pass_context
def cli(ctx):
    """ESP32-C6 flashing helper (c6_car).  No BOOT button needed - the board
    auto-resets into download mode and again after flashing."""
    if ctx.invoked_subcommand is None:
        ctx.invoke(full)


@cli.command()
@common_opts
def full(port, monitor):
    """Flash bootloader + partition table + otadata + firmware."""
    print(f"[c6] mode=full port={port}")
    flash_full(port)
    if monitor:
        do_monitor(port)


@cli.command()
@common_opts
def assets(port, monitor):
    """Repack assets_src/ and flash the control page only (firmware untouched)."""
    print(f"[c6] mode=assets port={port}")
    flash_assets(port)
    if monitor:
        do_monitor(port)


@cli.command(name="all")
@common_opts
def all_cmd(port, monitor):
    """Flash assets first, then full firmware (one power cycle for the user)."""
    print(f"[c6] mode=all port={port}")
    flash_assets(port)
    flash_full(port)
    if monitor:
        do_monitor(port)


@cli.command()
@click.argument("port", default=DEFAULT_PORT)
def mon(port):
    """Serial monitor only (115200, Ctrl+C to exit)."""
    do_monitor(port)


if __name__ == "__main__":
    cli()
