#!/usr/bin/env python3
"""Exercise authored fixtures in an ephemeral Linux VM with no disk/network/USB."""
import argparse
import gzip
from pathlib import Path
import stat
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def cpio(entries):
    data = bytearray()
    for ino, (name, content, mode) in enumerate(entries + [("TRAILER!!!", b"", 0)], 1):
        encoded = name.encode() + b"\0"
        fields = [ino, mode, 0, 0, 1, 0, len(content), 0, 0, 0, 0, len(encoded), 0]
        data.extend(b"070701" + "".join(f"{n:08x}" for n in fields).encode())
        data.extend(encoded)
        data.extend(b"\0" * (-len(data) % 4))
        data.extend(content)
        data.extend(b"\0" * (-len(data) % 4))
    return data


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("qemu", "kernel", "initramfs"):
        p.add_argument("--" + name, type=Path, required=True)
    p.add_argument("--arch", choices=["aarch64", "x86_64"], default="aarch64")
    p.add_argument("--build", type=Path)
    args = p.parse_args()
    build = (args.build or ROOT / "build/boot-probe" / args.arch).resolve()
    entries = [("boot-probe", b"", stat.S_IFDIR | 0o755)]
    for name in ("gps.default.so", "stock.so", "missing.so", "check"):
        entries.append(("boot-probe/" + name, (build / "tests" / name).read_bytes(), stat.S_IFREG | 0o755))
    init = b'''#!/bin/busybox sh
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
/bin/busybox --install -s /bin
mkdir -p /proc /sys /dev /tmp
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mount -t tmpfs tmpfs /tmp
ulimit -c 0
/boot-probe/check /boot-probe/gps.default.so /boot-probe/stock.so /boot-probe/missing.so
echo BOOT_PROBE_VM_EXIT=$?
poweroff -f
'''
    entries.append(("boot-probe/init", init, stat.S_IFREG | 0o755))
    image = build / "test-initramfs.gz"
    image.write_bytes(args.initramfs.read_bytes() + gzip.compress(cpio(entries), mtime=0))
    machine, console = ("virt", "ttyAMA0") if args.arch == "aarch64" else ("pc", "ttyS0")
    command = [str(args.qemu.resolve()), "-machine", machine, "-cpu", "max", "-accel", "tcg",
               "-m", "256", "-nographic", "-no-reboot", "-nic", "none",
               "-kernel", str(args.kernel.resolve()), "-initrd", str(image),
               "-append", f"console={console} rdinit=/boot-probe/init panic=-1 quiet"]
    try:
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        output = result.stdout.decode(errors="replace")
    except subprocess.TimeoutExpired as exc:
        output = (exc.stdout or b"").decode(errors="replace") + "\nVM_TIMEOUT\n"
    (build / "test-output.log").write_text(output, encoding="utf-8")
    print(output)
    if "BOOT_PROBE_VM_EXIT=0" not in output or "BOOT_PROBE_TESTS_PASSED=14" not in output:
        p.exit(1, "Boot-probe VM tests failed\n")


if __name__ == "__main__":
    main()
