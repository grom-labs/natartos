#!/usr/bin/env python3
"""Optional production ELF checks with private Android linker/libdl inputs."""
import argparse
import gzip
import os
from pathlib import Path
import stat
import subprocess
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"boot_probe"))
from run_vm_tests import cpio
ROOT=Path(__file__).resolve().parents[2]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ["zig","sysroot","qemu","kernel","initramfs"]:p.add_argument("--"+n,type=Path,required=True)
    a=p.parse_args();out=ROOT/"build/boot-activate/aarch64";test=out/"android";test.mkdir(exist_ok=True)
    env=os.environ.copy();env["ZIG_GLOBAL_CACHE_DIR"]=str(ROOT/"build/.zig-global-cache");env["ZIG_LOCAL_CACHE_DIR"]=str(ROOT/"build/.zig-local-cache")
    cc=[str(a.zig.resolve()),"cc","-target","aarch64-linux-musl","-std=c11","-O2","-Wall","-Wextra","-Werror","-nostdlib","-ffreestanding","-fno-builtin","-fno-stack-protector","-I",str(ROOT/"src/boot_activate")]
    shared=["-shared","-fPIC","-Wl,-z,noexecstack"]
    system=a.sysroot.resolve()/"system"
    executable=["-fPIE","-pie","-Wl,-e,_start,--export-dynamic,--dynamic-linker,/system/bin/linker64,--allow-shlib-undefined",str(system/"lib64/libdl.so")]
    def build(n,source,flags):subprocess.run(cc+flags+[str(ROOT/source),"-o",str(test/n)],check=True,env=env)
    build("gps-check","tests/boot_probe/android_check.c",executable+["-I",str(ROOT/"src/boot_probe")])
    build("loader-check","tests/boot_activate/android_loader_check.c",executable)
    build("stock.so","tests/boot_probe/stock.c",shared+["-Wl,-soname,gps.default.so"])
    build("overlay.so","tests/boot_activate/android_overlay.c",shared+["-Wl,-soname,hook_overlay.so"])
    directories=["system","system/bin","system/lib64","vendor","vendor/lib64","vendor/lib64/natartos_boot","vendor/natartos","vendor/natartos/mods","vendor/natartos/enabled","data","data/dji","data/dji/natartos-boot","trigger","boot-activate"]
    entries=[(d,b"",stat.S_IFDIR|0o755) for d in directories]
    files={"system/bin/linker64":system/"bin/linker64","system/lib64/libdl.so":system/"lib64/libdl.so",
           "system/bin/dji_sys":test/"gps-check","system/bin/dji_glasses":test/"loader-check",
           "vendor/lib64/gps.default.so":out/"gps.default.so","vendor/lib64/gps.default.stock.so":test/"stock.so",
           "vendor/lib64/natartos_boot/boot-supervisor":out/"trigger-helper",
           "vendor/natartos/natartos_loader.so":out/"natartos_loader.so","vendor/natartos/mods/hook_overlay.so":test/"overlay.so"}
    for n,path in files.items():entries.append((n,path.read_bytes(),stat.S_IFREG|0o755))
    for n,content in {"data/dji/natartos_boot_probe.log":b"","data/dji/natartos-boot/enabled":b"natartos-boot-v1\n","vendor/natartos/enabled/hook_overlay.so":b""}.items():entries.append((n,content,stat.S_IFREG|0o600))
    init=b'''#!/bin/busybox sh
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
/bin/busybox --install -s /bin
mkdir -p /proc /sys /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
ulimit -c 0
/system/bin/dji_sys
gps=$?
sleep 1
if [ "$(cat /trigger/result 2>/dev/null)" = ok ]; then echo ANDROID_BOOT_HANDOFF=PASS; fi
NATARTOS_BOOT_HEALTH_FD=198 NATARTOS_BOOT_HEALTH_NONCE=1234567890abcdef /system/bin/dji_glasses
loader=$?
echo ANDROID_BOOT_GPS_EXIT=$gps
echo ANDROID_BOOT_LOADER_EXIT=$loader
poweroff -f
'''
    entries.append(("boot-activate/init",init,stat.S_IFREG|0o755));image=test/"initramfs.gz";image.write_bytes(a.initramfs.read_bytes()+gzip.compress(cpio(entries),mtime=0))
    cmd=[str(a.qemu.resolve()),"-machine","virt","-cpu","max","-accel","tcg","-m","256","-nographic","-no-reboot","-nic","none","-kernel",str(a.kernel.resolve()),"-initrd",str(image.resolve()),"-append","console=ttyAMA0 rdinit=/boot-activate/init panic=-1 quiet"]
    try:r=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=60);output=r.stdout.decode(errors="replace")
    except subprocess.TimeoutExpired as e:output=(e.stdout or b"").decode(errors="replace")+"\nVM_TIMEOUT\n"
    (test/"output.log").write_text(output);print(output)
    if not all(s in output for s in ["ANDROID_BOOT_PROBE_SMOKE=PASS","ANDROID_BOOT_HANDOFF=PASS","ANDROID_BOOT_LOADER=PASS","ANDROID_BOOT_GPS_EXIT=0","ANDROID_BOOT_LOADER_EXIT=0"]):p.exit(1,"Android boot fixtures failed\n")
if __name__=="__main__":main()
