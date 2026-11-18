#!/usr/bin/env python3
"""Build/audit the experimental GPS marker proxy; never contacts a device."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def inspect_proxy(path, arch, test_variant=False):
    """Validate the ELF contract with only the Python standard library."""
    data = path.read_bytes()
    if data[:6] != b"\x7fELF\x02\x01":
        raise ValueError("Expected little-endian ELF64")
    machine = struct.unpack_from("<H", data, 18)[0]
    if machine != {"aarch64": 183, "x86_64": 62}[arch] or struct.unpack_from("<H", data, 16)[0] != 3:
        raise ValueError("Wrong proxy architecture/type")
    shoff = struct.unpack_from("<Q", data, 40)[0]
    shsize, count = struct.unpack_from("<HH", data, 58)
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shsize) for i in range(count)]

    def string(section, offset):
        start = section[4] + offset
        return data[start:data.index(b"\0", start)].decode()

    imports, exports, needed, init_bytes = [], [], [], 0
    for section in sections:
        if section[1] == 11:  # SHT_DYNSYM
            for offset in range(section[4], section[4] + section[5], section[9]):
                name, info, other, index, _, _ = struct.unpack_from("<IBBHQQ", data, offset)
                if not name:
                    continue
                value = string(sections[section[6]], name)
                if not index:
                    imports.append(value)
                elif info >> 4 in (1, 2) and other & 3 == 0:
                    exports.append(value)
        elif section[1] == 6:  # SHT_DYNAMIC
            for offset in range(section[4], section[4] + section[5], 16):
                tag, value = struct.unpack_from("<qQ", data, offset)
                if tag == 1:
                    needed.append(string(sections[section[6]], value))
        elif section[1] == 14:
            init_bytes += section[5]
    phoff = struct.unpack_from("<Q", data, 32)[0]
    phsize, phcount = struct.unpack_from("<HH", data, 54)
    stack_flags = []
    for i in range(phcount):
        kind, flags = struct.unpack_from("<II", data, phoff + i * phsize)
        if kind == 0x6474E551:
            stack_flags.append(flags)
        if kind == 1 and flags & 3 == 3:
            raise ValueError("Writable executable load segment")
    if sorted(imports) != ["dlopen", "dlsym"] or exports != ["gps_get_hardware_interface"]:
        raise ValueError(f"Unexpected proxy symbols: imports={imports} exports={exports}")
    if needed != ([] if test_variant else ["libdl.so"]) or init_bytes != 8 or len(stack_flags) != 1 or stack_flags[0] & 1:
        raise ValueError("Unexpected dependencies, constructor table or executable stack")
    return {"machine": machine, "imports": sorted(imports), "exports": exports,
            "needed": needed, "constructor_count": init_bytes // 8, "executable_stack": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zig", type=Path, help="Pinned Zig 0.15.2 executable")
    parser.add_argument("--cc", help="Compiler executable, default cc or aarch64-linux-gnu-gcc")
    parser.add_argument("--arch", choices=["aarch64", "x86_64"], default="aarch64")
    parser.add_argument("--tests", action="store_true", help="Also build Linux fixtures, never deployment artifacts")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    if args.zig and args.cc:
        parser.error("Choose --zig or --cc")
    out = (args.out or ROOT / "build/boot-probe" / args.arch).resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["ZIG_GLOBAL_CACHE_DIR"] = str(ROOT / "build/.zig-global-cache")
    env["ZIG_LOCAL_CACHE_DIR"] = str(ROOT / "build/.zig-local-cache")
    if args.zig:
        version = subprocess.check_output([str(args.zig.resolve()), "version"], text=True).strip()
        if version != "0.15.2":
            parser.error(f"Expected Zig 0.15.2, found {version}")
        cc = [str(args.zig.resolve()), "cc", "-target", f"{args.arch}-linux-musl"]
        compiler = f"Zig {version}"
    else:
        cc = [args.cc or ("cc" if args.arch == "x86_64" else "aarch64-linux-gnu-gcc")]
        compiler = subprocess.check_output(cc + ["--version"], text=True).splitlines()[0]
    common = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O2"]
    shared = ["-shared", "-fPIC", "-nostdlib", "-ffreestanding", "-fno-builtin",
              "-fno-stack-protector", "-Wl,-z,noexecstack", "-Wl,-z,relro,-z,now",
              "-Wl,-Bsymbolic", "-Wl,-soname,gps.default.so"]
    source = ROOT / "src/boot_probe/gps_forward.c"

    def build(path, sources, flags):
        subprocess.run(cc + common + flags + [str(s) for s in sources] + ["-o", str(path)],
                       check=True, env=env)

    # This authored ELF interface is only a linker input. It is deliberately
    # stored away from the production artifact and must never be installed.
    link_only = out / "link-only"
    link_only.mkdir(exist_ok=True)
    interface = link_only / "libdl.so"
    build(interface, [ROOT / "tools/boot_probe/libdl_import.c"],
          [f for f in shared if not f.startswith("-Wl,-soname,")] + ["-Wl,-soname,libdl.so"])
    build(out / "gps.default.so", [source], shared + ["-fvisibility=hidden", "-Wl,--no-as-needed", str(interface)])
    elf = inspect_proxy(out / "gps.default.so", args.arch)
    if args.tests:
        tests = out / "tests"
        tests.mkdir(exist_ok=True)
        build(tests / "gps.default.so", [source], shared + ["-fvisibility=hidden", "-DNATARTOS_BOOT_PROBE_TEST"])
        # Linux fixtures resolve these calls through their host libc. Android
        # dependency resolution is tested separately with the production file.
        inspect_proxy(tests / "gps.default.so", args.arch, test_variant=True)
        build(tests / "stock.so", [ROOT / "tests/boot_probe/stock.c"], shared)
        build(tests / "missing.so", [ROOT / "tests/boot_probe/stock.c"], shared + ["-DSTOCK_MISSING_SYMBOL"])
        build(tests / "check", [ROOT / "tests/boot_probe/check.c"],
              (["-dynamic"] if args.zig else []) + ["-ldl", "-pthread"])
    sources = [source, ROOT / "src/boot_probe/raw_syscall.h", Path(__file__).resolve(),
               ROOT / "tools/boot_probe/libdl_import.c"]
    info = {"experimental": True, "device_tested": False, "arch": args.arch,
            "compiler": compiler, "elf": elf,
            "stock_path": "/vendor/lib64/gps.default.stock.so",
            "marker_path": "/data/dji/natartos_boot_probe.log",
            "sources": {str(p.relative_to(ROOT).as_posix()): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
            "artifacts": {"gps.default.so": hashlib.sha256((out / "gps.default.so").read_bytes()).hexdigest()}}
    (out / "build-info.json").write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")
    print(f"Built and ELF-audited {out / 'gps.default.so'}")


if __name__ == "__main__":
    main()
