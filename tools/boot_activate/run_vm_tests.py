#!/usr/bin/env python3
"""Run boot-activation fixtures in a diskless, networkless, USB-less VM."""
import argparse
import gzip
from pathlib import Path
import stat
import subprocess
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"boot_probe"))
from run_vm_tests import cpio
ROOT=Path(__file__).resolve().parents[2]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ["qemu","kernel","initramfs"]:p.add_argument("--"+n,type=Path,required=True)
    p.add_argument("--arch",choices=["aarch64","x86_64"],default="aarch64")
    a=p.parse_args();out=ROOT/"build/boot-activate"/a.arch
    entries=[("boot-activate",b"",stat.S_IFDIR|0o755)]
    for n in ["profile-check","supervisor-check","test-supervisor","test-qualify","fake-gui","fake-service","trigger-check","trigger-helper"]:entries.append(("boot-activate/"+n,(out/n).read_bytes(),stat.S_IFREG|0o755))
    init=b'''#!/bin/busybox sh
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
/bin/busybox --install -s /bin
mkdir -p /proc /sys /dev /tmp
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mount -t tmpfs tmpfs /tmp
ulimit -c 0
/boot-activate/profile-check
profile=$?
/boot-activate/supervisor-check /boot-activate/test-supervisor /boot-activate/fake-gui /boot-activate/fake-service /boot-activate/test-qualify
supervisor=$?
/boot-activate/trigger-check /boot-activate/trigger-helper
trigger=$?
echo BOOT_ACTIVATION_PROFILE_EXIT=$profile
echo BOOT_ACTIVATION_VM_EXIT=$supervisor
echo BOOT_ACTIVATION_TRIGGER_EXIT=$trigger
poweroff -f
'''
    entries.append(("boot-activate/init",init,stat.S_IFREG|0o755))
    image=out/"test-initramfs.gz";image.write_bytes(a.initramfs.read_bytes()+gzip.compress(cpio(entries),mtime=0))
    machine,console=("virt","ttyAMA0") if a.arch=="aarch64" else ("pc","ttyS0")
    cmd=[str(a.qemu.resolve()),"-machine",machine,"-cpu","max","-accel","tcg","-m","256","-nographic","-no-reboot","-nic","none","-kernel",str(a.kernel.resolve()),"-initrd",str(image.resolve()),"-append",f"console={console} rdinit=/boot-activate/init panic=-1 quiet"]
    try:r=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=60);output=r.stdout.decode(errors="replace")
    except subprocess.TimeoutExpired as e:output=(e.stdout or b"").decode(errors="replace")+"\nVM_TIMEOUT\n"
    (out/"test-output.log").write_text(output)
    print(output)
    if not all(marker in output for marker in ["BOOT_PROFILE_TESTS_PASSED","BOOT_SUPERVISOR_TESTS_PASSED=25","BOOT_TRIGGER_TESTS_PASSED=7","BOOT_ACTIVATION_PROFILE_EXIT=0","BOOT_ACTIVATION_VM_EXIT=0","BOOT_ACTIVATION_TRIGGER_EXIT=0"]):p.exit(1,"Activation fixtures failed\n")
if __name__=="__main__":main()
