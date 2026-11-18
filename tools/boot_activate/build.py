#!/usr/bin/env python3
"""Build the opt-in experimental boot components; no device access."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from elf import inspect
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"boot_probe"))
from build import inspect_proxy

ROOT = Path(__file__).resolve().parents[2]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--zig",type=Path,required=True)
    p.add_argument("--arch",choices=["aarch64","x86_64"],default="aarch64")
    args=p.parse_args()
    zig=str(args.zig.resolve())
    if subprocess.check_output([zig,"version"],text=True).strip()!="0.15.2":p.error("Zig 0.15.2 required")
    env=os.environ.copy()
    env["ZIG_GLOBAL_CACHE_DIR"]=str(ROOT/"build/.zig-global-cache")
    env["ZIG_LOCAL_CACHE_DIR"]=str(ROOT/"build/.zig-local-cache")
    out=ROOT/"build/boot-activate"/args.arch;out.mkdir(parents=True,exist_ok=True)
    src=ROOT/"src/boot_activate"
    cc=[zig,"cc","-target",args.arch+"-linux-musl","-std=c11","-O2","-Wall","-Wextra","-Werror","-static","-I",str(src),"-Wl,-z,noexecstack"]
    def build(name,sources,flags=()):
        subprocess.run(cc+list(flags)+[str(s) for s in sources]+["-o",str(out/name)],check=True,env=env)
    build("boot-preflight",[src/"preflight.c",src/"profile.c"],["-DNATARTOS_BOOT_READ_ONLY"])
    build("boot-supervisor",[src/"supervisor.c",src/"profile.c"])
    build("boot-qualify",[src/"supervisor.c",src/"profile.c"],["-DNATARTOS_BOOT_QUALIFY","-DNATARTOS_BOOT_READ_ONLY","-Wno-unused-function"])
    build("test-supervisor",[src/"supervisor.c",src/"profile.c"],["-DNATARTOS_BOOT_TEST"])
    build("test-qualify",[src/"supervisor.c",src/"profile.c"],["-DNATARTOS_BOOT_TEST","-DNATARTOS_BOOT_QUALIFY","-DNATARTOS_BOOT_READ_ONLY","-Wno-unused-function"])
    build("profile-check",[ROOT/"tests/boot_activate/profile_check.c",src/"profile.c"],["-DNATARTOS_BOOT_TEST"])
    build("supervisor-check",[ROOT/"tests/boot_activate/supervisor_check.c",src/"profile.c"],["-DNATARTOS_BOOT_TEST"])
    build("fake-gui",[ROOT/"tests/boot_activate/fake_gui.c"])
    build("fake-service",[ROOT/"tests/boot_activate/fake_service.c"])
    build("trigger-check",[ROOT/"tests/boot_activate/trigger_check.c"],["-DNATARTOS_BOOT_TRIGGER_TEST"])
    build("trigger-helper",[ROOT/"tests/boot_activate/trigger_helper.c"])
    names=["boot-preflight","boot-supervisor","boot-qualify","test-supervisor","test-qualify","profile-check","supervisor-check","fake-gui","fake-service","trigger-check","trigger-helper"]
    audit={}
    for name in names:
        audit[name]=inspect(out/name,args.arch)
        if audit[name]["imports"] or audit[name]["needed"] or audit[name]["interpreter"]:raise ValueError("Expected static native helper")
    for name in ["boot-supervisor","boot-qualify"]:
        if b"/dev/input/" in (out/name).read_bytes() or b"gpio_keys" in (out/name).read_bytes():raise ValueError("Boot helper unexpectedly includes input-device access")
    if b"dji.glasses_service" in (out/"boot-qualify").read_bytes():raise ValueError("Qualifier unexpectedly includes UI control")
    if args.arch=="aarch64":
        shared=["-shared","-fPIC","-nostdlib","-ffreestanding","-fno-builtin","-fno-stack-protector","-Wl,-Bsymbolic","-Wl,-z,noexecstack,-z,relro,-z,now"]
        link=out/"link-only";link.mkdir(exist_ok=True)
        def so(name,sources,flags):
            subprocess.run([v for v in cc if v!="-static"]+shared+flags+[str(s) for s in sources]+["-o",str(out/name)],check=True,env=env)
        so("link-only/libdl.so",[ROOT/"tools/boot_probe/libdl_import.c"],["-Wl,-soname,libdl.so"])
        so("gps.default.so",[ROOT/"src/boot_probe/gps_forward.c"],["-DNATARTOS_BOOT_ACTIVATION","-fvisibility=hidden","-Wl,-soname,gps.default.so","-Wl,--no-as-needed",str(link/"libdl.so")])
        inspect_proxy(out/"gps.default.so",args.arch)
        loader_flags=["-fvisibility=hidden","-Wl,-soname,natartos_loader.so","-Wl,--no-as-needed",str(link/"libdl.so")]
        so("natartos_loader.so",[ROOT/"src/natartos_loader.c"],["-DNATARTOS_BOOT_ACTIVATION",*loader_flags])
        audit["natartos_loader.so"]=inspect(out/"natartos_loader.so",args.arch)
        if audit["natartos_loader.so"]["imports"]!=["dlsym"] or audit["natartos_loader.so"]["needed"]!=["libdl.so"] or audit["natartos_loader.so"]["exports"]:raise ValueError("Unexpected loader ABI")
        manual=out/"manual";manual.mkdir(exist_ok=True)
        so("manual/natartos_loader.so",[ROOT/"src/natartos_loader.c"],loader_flags)
        audit["manual/natartos_loader.so"]=inspect(manual/"natartos_loader.so",args.arch)
        if b"NATARTOS_BOOT_HEALTH" in (manual/"natartos_loader.so").read_bytes():raise ValueError("Default loader includes experimental boot health")
        if audit["manual/natartos_loader.so"]["imports"]!=["dlsym"] or audit["manual/natartos_loader.so"]["needed"]!=["libdl.so"] or audit["manual/natartos_loader.so"]["exports"]:raise ValueError("Unexpected default loader ABI")
        names += ["gps.default.so","natartos_loader.so","manual/natartos_loader.so"]
    artifacts={n:hashlib.sha256((out/n).read_bytes()).hexdigest() for n in names}
    info={"experimental":True,"device_tested":False,"arch":args.arch,"artifacts":artifacts,"elf":audit,
          "sources":{str(f.relative_to(ROOT).as_posix()):hashlib.sha256(f.read_bytes()).hexdigest() for f in [*src.glob("*"),*(ROOT/"src/boot_probe").glob("*"),ROOT/"src/natartos_loader.c",*(ROOT/"tests/boot_activate").glob("*"),*Path(__file__).resolve().parent.glob("*.py")] if f.is_file()}}
    (out/"build-info.json").write_text(json.dumps(info,indent=2)+"\n")
    print("Built",out)
if __name__=="__main__":main()
