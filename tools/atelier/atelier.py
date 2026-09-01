#!/usr/bin/env python3
"""
atelier — where complications are built.

Packs a complication directory (app.js + manifest.json) into a .comp
package and optionally pushes it to a device running the Kaliber sync
endpoint. Designed to slot into an existing release.sh pipeline.

Usage:
    atelier.py pack  <appdir> [-o out.comp] [--key hexkey]
    atelier.py push  <pkg.comp> --host watchy.local [--port 8080]

Package format (tar, no compression — LittleFS-friendly):
    manifest.json
    app.qjb          QuickJS bytecode   (if qjsc available / requested)
    app.mqb          MQuickJS bytecode  (if mqjs compiler available)
    sig.hmac         hex HMAC-SHA256 over manifest.json + bytecode files

Compilers are located via $QJSC and $MQJSC or PATH. ABI version below must
match KB_APP_ABI_VERSION in app_store.h — bump both together.
"""
import argparse
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
    """qjsc -c emits a C file; extract raw bytecode via -o with binary mode
    if available, else fall back to the C-array parse."""
    qjsc = find_compiler("QJSC", ["qjsc"])
    if not qjsc:
        return False
    with tempfile.NamedTemporaryFile(suffix=".c", delete=False) as tf:
        cfile = tf.name
    try:
        subprocess.run([qjsc, "-c", "-o", cfile, src], check=True)
        data = parse_c_array(open(cfile).read())
        with open(out, "wb") as f:
            f.write(data)
        return True
    finally:
        os.unlink(cfile)


def parse_c_array(c_src: str) -> bytes:
    """Extract the byte array qjsc emits (uint8_t xxx[] = {...})."""
    start = c_src.index("{") + 1
    end = c_src.index("}", start)
    return bytes(
        int(tok, 0)
        for tok in c_src[start:end].replace("\n", " ").split(",")
        if tok.strip()
    )


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

        payload = open(mpath, "rb").read()
        for e in sorted(entries.values()):
            payload += open(os.path.join(tmp, e), "rb").read()
        if args.key:
            sig = hmac.new(bytes.fromhex(args.key), payload,
                           hashlib.sha256).hexdigest()
            open(os.path.join(tmp, "sig.hmac"), "w").write(sig)

        with tarfile.open(out, "w") as tar:
            for name in ["manifest.json", *sorted(entries.values())] + (
                ["sig.hmac"] if args.key else []
            ):
                tar.add(os.path.join(tmp, name), arcname=name)

    engines = "+".join(sorted(entries))
    print(f"packed {out} ({engines}, abi {ABI_VERSION})")


def cmd_push(args: argparse.Namespace) -> None:
    data = open(args.package, "rb").read()
    url = f"http://{args.host}:{args.port}/install"
    req = urllib.request.Request(
        url, data=data, method="POST",
        headers={"Content-Type": "application/x-kaliber-comp"},
    )
    with urllib.request.urlopen(req, timeout=30) as resp:
        print(f"push: {resp.status} {resp.read().decode().strip()}")


def main() -> None:
    p = argparse.ArgumentParser(prog="atelier")
    sub = p.add_subparsers(dest="cmd", required=True)

    pk = sub.add_parser("pack", help="build a .comp package")
    pk.add_argument("appdir")
    pk.add_argument("-o", "--output")
    pk.add_argument("--key", help="hex HMAC key (matches firmware)")
    pk.set_defaults(func=cmd_pack)

    ps = sub.add_parser("push", help="upload to device sync endpoint")
    ps.add_argument("package")
    ps.add_argument("--host", required=True)
    ps.add_argument("--port", type=int, default=8080)
    ps.set_defaults(func=cmd_push)

    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
