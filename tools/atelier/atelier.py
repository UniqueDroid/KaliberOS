#!/usr/bin/env python3
"""
atelier — where complications are built.

Packs a complication directory (app.js + manifest.json) into a .comp
package and optionally pushes it to a device running the Kaliber sync
endpoint. Designed to slot into an existing release.sh pipeline.

Usage:
    atelier.py pack  <appdir> [-o out.comp]
    atelier.py push  <pkg.comp> --host watchy.local [--port 8080] --key hexkey

Package format (tar, no compression — LittleFS-friendly):
    manifest.json
    app.qjb          QuickJS bytecode   (if qjsc available / requested)
    app.mqb          MQuickJS bytecode  (if mqjs compiler available)
    sig              "<scheme>:<key-id>:<hex-signature>", added by `push`

`pack` never signs (docs/design/package-signing.md): a .comp it produces
is a plain, unsigned, freely distributable artifact - the same file
works for any target device. Signing happens at `push` time, over
*this* device's key, which is what actually decides "may this be
installed here" (a device-key HMAC answers only that question, not
"who built this package" - see the design doc for why that distinction
matters and what a real provenance check would need instead). --key is
read off the target device's own sync screen (docs/design/
package-signing.md's pairing flow) - the same key `push`'s own /install
call already implicitly trusted before this split, just made explicit
now instead of baked into the package ahead of time.

Compilers are located via $QJSC and $MQJSC or PATH. ABI version below must
match KB_APP_ABI_VERSION in app_store.h — bump both together.
"""
import argparse
import calendar
import hashlib
import hmac
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request

ABI_VERSION = 1
REQUIRED_MANIFEST = ("id", "version", "type")


def die(msg: str) -> "NoReturn":
    print(f"atelier: {msg}", file=sys.stderr)
    sys.exit(1)


def find_compiler(env: str, names: list[str]) -> str | None:
    if os.environ.get(env):
        return os.environ[env]
    for n in names:
        if shutil.which(n):
            return n
    return None


def compile_quickjs(src: str, out: str) -> bool:
    """-s -b emits raw bytecode directly - the same invocation used
    throughout the project (see main/hello_bytecode.h's header comment).
    An earlier version of this function went via 'qjsc -c' (emit a .c
    file, parse the byte array back out) - that flag doesn't exist on the
    quickjs-ng qjsc this project actually builds (third_party/quickjs,
    v0.16.2); 'qjsc -c ...' just printed usage and exited 1. Found by
    actually running this against kb_store_install(), not assumed."""
    qjsc = find_compiler("QJSC", ["qjsc"])
    if not qjsc:
        return False
    subprocess.run([qjsc, "-s", "-b", "-o", out, src], check=True)
    return True


def compile_mquickjs(src: str, out: str) -> bool:
    mqjsc = find_compiler("MQJSC", ["mqjsc", "mqjs-compile"])
    if not mqjsc:
        return False
    subprocess.run([mqjsc, "-o", out, src], check=True)
    return True


def load_manifest(appdir: str) -> dict:
    path = os.path.join(appdir, "manifest.json")
    if not os.path.exists(path):
        die(f"no manifest.json in {appdir}")
    mf = json.load(open(path))
    for k in REQUIRED_MANIFEST:
        if k not in mf:
            die(f"manifest missing '{k}'")
    mf["abi"] = ABI_VERSION
    return mf


def cmd_pack(args: argparse.Namespace) -> None:
    appdir = args.appdir
    src = os.path.join(appdir, "app.js")
    if not os.path.exists(src):
        die(f"no app.js in {appdir}")

    mf = load_manifest(appdir)
    out = args.output or f"{mf['id']}-{mf['version']}.comp"

    with tempfile.TemporaryDirectory() as tmp:
        entries: dict[str, str] = {}
        qjb = os.path.join(tmp, "app.qjb")
        mqb = os.path.join(tmp, "app.mqb")
        if compile_quickjs(src, qjb):
            entries["quickjs"] = "app.qjb"
        if compile_mquickjs(src, mqb):
            entries["mquickjs"] = "app.mqb"
        if not entries:
            die("no engine compiler found (set $QJSC and/or $MQJSC)")
        mf["entries"] = entries

        mpath = os.path.join(tmp, "manifest.json")
        json.dump(mf, open(mpath, "w"), indent=2)

        with tarfile.open(out, "w") as tar:
            for name in ["manifest.json", *sorted(entries.values())]:
                tar.add(os.path.join(tmp, name), arcname=name)

    engines = "+".join(sorted(entries))
    print(f"packed {out} ({engines}, abi {ABI_VERSION}) - unsigned, sign at push time")


def sign_package(package_path: str, key_hex: str) -> bytes:
    """Reads an unsigned .comp, signs manifest.json + bytecode entries
    (sorted by name, same order install_impl() on the device verifies
    in) with the given device key, and returns a new tar's bytes with a
    "sig" entry appended - the actual push-time signing step
    docs/design/package-signing.md moved here from `pack`. Rejects (does
    not silently re-sign) a package that already has a `sig` entry -
    that would mean pushing something someone else already signed for a
    *different* device, almost certainly not what was intended."""
    key = bytes.fromhex(key_hex)
    with tarfile.open(package_path, "r") as tar:
        names = tar.getnames()
        if "sig" in names:
            die(f"{package_path} is already signed - pack produces unsigned "
                f".comp files, sign exactly once, at push time, per device")
        if "manifest.json" not in names:
            die(f"{package_path}: no manifest.json - not a valid .comp")
        entries = {n: tar.extractfile(n).read() for n in names}

    payload = entries["manifest.json"]
    bytecode_names = sorted(n for n in entries if n != "manifest.json")
    for n in bytecode_names:
        payload += entries[n]
    sig_hex = hmac.new(key, payload, hashlib.sha256).hexdigest()
    sig_field = f"hmac-sha256:default:{sig_hex}".encode()

    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as tar:
        for name in ["manifest.json", *bytecode_names]:
            info = tarfile.TarInfo(name)
            info.size = len(entries[name])
            tar.addfile(info, io.BytesIO(entries[name]))
        info = tarfile.TarInfo("sig")
        info.size = len(sig_field)
        tar.addfile(info, io.BytesIO(sig_field))
    return buf.getvalue()


def cmd_push(args: argparse.Namespace) -> None:
    data = sign_package(args.package, args.key)
    url = f"http://{args.host}:{args.port}/install"
    req = urllib.request.Request(
        url, data=data, method="POST",
        headers={"Content-Type": "application/x-kaliber-comp"},
    )
    with urllib.request.urlopen(req, timeout=30) as resp:
        print(f"push: {resp.status} {resp.read().decode().strip()}")

    # Piggybacks the pushing host's own clock onto every push (project
    # chat 2026-09-05) - watchy_v3 has no RTC chip and nothing else sets
    # the device's clock yet (no SNTP), so without this every push would
    # leave the watch showing "--:--" (cadran/providers.c's time_set
    # check) even though it's sitting right next to a computer that knows
    # the time perfectly well. Best-effort: a device that's otherwise
    # working shouldn't fail the whole push over this alone.
    #
    # calendar.timegm(time.localtime()), not time.time(): nothing in this
    # firmware ever calls tzset()/setenv("TZ", ...), so the device's own
    # localtime_r() treats whatever epoch it's given as UTC directly -
    # sending the real UTC epoch would show UTC wall-clock time on the
    # panel, off by the host's own UTC offset (e.g. 2h for CEST). This
    # reinterprets the host's local broken-down time *as if* it were UTC,
    # producing the epoch that makes the device's no-timezone arithmetic
    # land on the same digits the host's own clock shows - the standard
    # trick for a device with no timezone database at all.
    time_url = f"http://{args.host}:{args.port}/time"
    time_req = urllib.request.Request(
        time_url, data=str(calendar.timegm(time.localtime())).encode(), method="POST",
    )
    try:
        with urllib.request.urlopen(time_req, timeout=10) as resp:
            print(f"time: {resp.status} {resp.read().decode().strip()}")
    except OSError as e:
        print(f"atelier: warning: could not set device time: {e}", file=sys.stderr)


def main() -> None:
    p = argparse.ArgumentParser(prog="atelier")
    sub = p.add_subparsers(dest="cmd", required=True)

    pk = sub.add_parser("pack", help="build an unsigned .comp package")
    pk.add_argument("appdir")
    pk.add_argument("-o", "--output")
    pk.set_defaults(func=cmd_pack)

    ps = sub.add_parser("push", help="sign for the target device and upload")
    ps.add_argument("package")
    ps.add_argument("--host", required=True)
    ps.add_argument("--port", type=int, default=8080)
    ps.add_argument("--key", required=True,
                     help="hex device key, read off the target's sync screen")
    ps.set_defaults(func=cmd_push)

    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
