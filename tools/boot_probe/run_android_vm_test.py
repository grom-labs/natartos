#!/usr/bin/env python3
"""Optional VM smoke test with a privately supplied Android ARM64 linker/libdl.

Uses authored stock GPS fixtures, never the vendor GPS implementation. Does
not install, run DJI services, connect USB, or ship the supplied Android files.
"""
import argparse
import gzip
import os
from pathlib import Path
import stat
import subprocess
from run_vm_tests import cpio
from build import inspect_proxy

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("zig", "sysroot", "qemu", "kernel", "initramfs"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--build", type=Path, default=ROOT / "build/boot-probe/aarch64")
    args = parser.parse_args()
    out = args.build.resolve()
    inspect_proxy(out / "gps.default.so", "aarch64")
    system = args.sysroot.resolve() / "system"
    env = os.environ.copy()
    env["ZIG_GLOBAL_CACHE_DIR"] = str(ROOT / "build/.zig-global-cache")
    env["ZIG_LOCAL_CACHE_DIR"] = str(ROOT / "build/.zig-local-cache")
    command = [str(args.zig.resolve()), "cc", "-target", "aarch64-linux-musl", "-O2",
               "-Wall", "-Wextra", "-Werror", "-fPIE", "-nostdlib", "-ffreestanding",
               "-fno-builtin", "-fno-stack-protector", "-Wl,-e,_start", "-pie",
               "-Wl,--dynamic-linker,/system/bin/linker64", "-Wl,--allow-shlib-undefined",
               "-I", str(ROOT / "src/boot_probe"), str(ROOT / "tests/boot_probe/android_check.c"),
               str(system / "lib64/libdl.so"), "-o", str(out / "android_check")]
    subprocess.run(command, check=True, env=env)
    entries = [(d, b"", stat.S_IFDIR | 0o755) for d in
               ("system", "system/bin", "system/lib64", "vendor", "vendor/lib64", "data", "data/dji", "boot-probe")]
    for name, path in {
        "system/bin/linker64": system / "bin/linker64",
        "system/lib64/libdl.so": system / "lib64/libdl.so",
        "vendor/lib64/gps.default.so": out / "gps.default.so",
        "vendor/lib64/gps.default.stock.so": out / "tests/stock.so",
        "boot-probe/android_check": out / "android_check",
    }.items():
        entries.append((name, path.read_bytes(), stat.S_IFREG | 0o755))
    entries.append(("data/dji/natartos_boot_probe.log", b"", stat.S_IFREG | 0o600))
    init = b'''#!/bin/busybox sh
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
/bin/busybox --install -s /bin
mkdir -p /proc /sys /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
ulimit -c 0
/boot-probe/android_check
echo ANDROID_BOOT_PROBE_EXIT=$?
poweroff -f
'''
    entries.append(("boot-probe/init", init, stat.S_IFREG | 0o755))
    image = out / "android-test-initramfs.gz"
    image.write_bytes(args.initramfs.read_bytes() + gzip.compress(cpio(entries), mtime=0))
    vm = [str(args.qemu.resolve()), "-machine", "virt", "-cpu", "max", "-accel", "tcg", "-m", "256",
          "-nographic", "-no-reboot", "-nic", "none", "-kernel", str(args.kernel.resolve()),
          "-initrd", str(image), "-append", "console=ttyAMA0 rdinit=/boot-probe/init panic=-1 quiet"]
    try:
        result = subprocess.run(vm, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        output = result.stdout.decode(errors="replace")
    except subprocess.TimeoutExpired as exc:
        output = (exc.stdout or b"").decode(errors="replace") + "\nVM_TIMEOUT\n"
    (out / "android-test-output.log").write_text(output, encoding="utf-8")
    print(output)
    if "ANDROID_BOOT_PROBE_SMOKE=PASS" not in output or "ANDROID_BOOT_PROBE_EXIT=0" not in output:
        parser.exit(1, "Android-linker fixture failed\n")


if __name__ == "__main__":
    main()
