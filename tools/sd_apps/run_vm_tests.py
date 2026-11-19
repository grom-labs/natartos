#!/usr/bin/env python3
"""Run the Linux integration tests in an ephemeral QEMU guest (no disk/network)."""
import argparse
import gzip
import os
from pathlib import Path
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def cpio(entries):
    result = bytearray()
    for ino, (name, content, mode) in enumerate(entries + [("TRAILER!!!", b"", 0)], 1):
        encoded = name.encode() + b"\0"
        fields = [ino, mode, 0, 0, 1, 0, len(content), 0, 0, 0, 0, len(encoded), 0]
        result.extend(b"070701" + "".join(f"{v:08x}" for v in fields).encode())
        result.extend(encoded)
        result.extend(b"\0" * (-len(result) % 4))
        result.extend(content)
        result.extend(b"\0" * (-len(result) % 4))
    return bytes(result)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--qemu", type=Path, required=True)
    p.add_argument("--kernel", type=Path, required=True)
    p.add_argument("--initramfs", type=Path, required=True)
    p.add_argument("--build", type=Path, default=ROOT / "build/host")
    p.add_argument("--arch", choices=["x86_64", "aarch64"], default="x86_64")
    args = p.parse_args()
    build = args.build.resolve()
    entries = [("sd-tests", b"", stat.S_IFDIR | 0o755)]
    for item in sorted(build.rglob("*")):
        # Only include product binaries/packages, not toolchain caches.
        relative = item.relative_to(build)
        if relative.parts[0] not in {"apps", "fixture", "hash_check", "mod_check", "overlay_fixture", "sd_app_runner"}:
            continue
        name = "sd-tests/build/" + relative.as_posix()
        if item.is_dir(): entries.append((name, b"", stat.S_IFDIR | 0o755))
        elif item.is_file(): entries.append((name, item.read_bytes(), stat.S_IFREG | 0o755))
    entries.insert(1, ("sd-tests/build", b"", stat.S_IFDIR | 0o755))
    entries.append(("sd-tests/test_runner.sh", (ROOT / "tests/sd_apps/test_runner.sh").read_bytes(), stat.S_IFREG | 0o755))
    init = b'''#!/bin/busybox sh
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
/bin/busybox --install -s /bin
chmod 755 /
mkdir -p /proc /sys /dev /work
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mount -t tmpfs -o size=384m tmpfs /work
ulimit -c 0
TMPDIR=/work sh /sd-tests/test_runner.sh /sd-tests/build
result=$?
echo SD_APPS_VM_EXIT=$result
poweroff -f
'''
    entries.append(("sd-tests/init", init, stat.S_IFREG | 0o755))
    out = ROOT / "build/vm"
    out.mkdir(parents=True, exist_ok=True)
    image = out / f"test-initramfs-{args.arch}.gz"
    image.write_bytes(args.initramfs.read_bytes() + gzip.compress(cpio(entries), mtime=0))
    machine, cpu, console = ("virt", "cortex-a53", "ttyAMA0") if args.arch == "aarch64" else ("pc", "max", "ttyS0")
    command = [str(args.qemu.resolve())]
    command += ["-machine", machine, "-accel", "tcg", "-cpu", cpu, "-m", "512", "-nographic",
                "-no-reboot", "-nic", "none", "-kernel", str(args.kernel.resolve()), "-initrd", str(image),
                "-append", f"console={console} rdinit=/sd-tests/init panic=-1 quiet"]
    env = os.environ.copy()
    try:
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=150, env=env)
        output = result.stdout.decode(errors="replace")
    except subprocess.TimeoutExpired as exc:
        output = (exc.stdout or b"").decode(errors="replace")
        print(output)
        p.exit(1, "VM test timeout\n")
    (out / f"test-output-{args.arch}.log").write_text(output, encoding="utf-8")
    print(output)
    if "SD_APPS_VM_EXIT=0" not in output or "SD_APPS_TESTS_PASSED=" not in output:
        sys.exit(1)


if __name__ == "__main__":
    main()
