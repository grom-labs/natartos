#!/usr/bin/env python3
"""Build the standalone runner and example without installing anything on a device."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BASE_REVISION = "82f1a7da1748f690eb1031bb391666ec09508278"


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--zig", type=Path, help="Zig 0.15.2 executable (optional; otherwise use GCC)")
    p.add_argument("--target", default="aarch64-linux-musl", choices=["aarch64-linux-musl", "x86_64-linux-musl"])
    p.add_argument("--host-test", action="store_true", help="accept the build architecture; never deploy this runner")
    p.add_argument("--out", type=Path)
    p.add_argument("--framework", action="store_true", help="also compile the upstream loader/template baseline (ARM64 only)")
    args = p.parse_args()
    out = (args.out or ROOT / "build" / ("host" if args.host_test else "aarch64")).resolve()
    if args.framework and args.target != "aarch64-linux-musl":
        p.error("framework baseline requires ARM64")
    if args.zig:
        version = subprocess.check_output([str(args.zig.resolve()), "version"], text=True).strip()
        if version != "0.15.2":
            p.error(f"expected pinned Zig 0.15.2, found {version}")
        cc = [str(args.zig.resolve()), "cc", "-target", args.target]
        compiler = f"Zig {version}"
    else:
        cc = ["cc" if args.host_test else "aarch64-linux-gnu-gcc"]
        compiler = subprocess.check_output(cc + ["--version"], text=True).splitlines()[0]
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["ZIG_GLOBAL_CACHE_DIR"] = str(ROOT / "build/.zig-global-cache")
    env["ZIG_LOCAL_CACHE_DIR"] = str(ROOT / "build/.zig-local-cache")

    def build(dest, sources, extra=()):
        dest.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(cc + ["-O2", "-std=c11", "-Wall", "-Wextra", "-Werror", "-static", "-no-pie", "-s"]
                       + list(extra) + [str(ROOT / s) for s in sources] + ["-o", str(dest)], check=True, env=env)

    def build_mod(name, source):
        # Same flags as the CI mod loop: self-contained, loaded into dji_glasses.
        subprocess.run(cc + ["-shared", "-fPIC", "-O2", "-fno-builtin", "-nostdlib",
                            "-I", str(ROOT / "include"), "-I", str(ROOT / "mods"), f"-Wl,-soname,{name}",
                            str(ROOT / source), "-o", str(out / name)], check=True, env=env)

    include = ["-I", str(ROOT / "include")]
    flags = ["-DSD_APPS_HOST_TEST"] if args.host_test else []
    build(out / "sd_app_runner", ["mods/sd_apps/runner.c", "mods/sd_apps/package.c", "mods/sd_apps/sha256.c"],
          flags + include)
    build(out / "apps/hello/bin/hello", ["examples/sd_apps/hello/hello.c"], include)
    shutil.copyfile(ROOT / "examples/sd_apps/hello/app.json", out / "apps/hello/app.json")
    (out / "apps/hello/assets").mkdir(exist_ok=True)
    shutil.copyfile(ROOT / "examples/sd_apps/hello/assets/message.txt", out / "apps/hello/assets/message.txt")
    if args.host_test:
        build(out / "fixture", ["tests/sd_apps/fixture.c"])
        build(out / "hash_check", ["tests/sd_apps/hash_check.c", "mods/sd_apps/sha256.c"])
        # The SD Apps mod's logic, against a libc stand-in for natartos_common.h.
        build(out / "mod_check", ["tests/sd_apps/mod_check.c"], ["-I", str(ROOT / "tests/sd_apps/shim")])
        build(out / "overlay_fixture", ["tests/sd_apps/overlay_fixture.c"], include)
    else:
        build_mod("mod_sd_apps.so", "mods/sd_apps/mod.c")
        build_mod("hook_menu.so", "mods/menu/mod.c")
        build_mod("hook_overlay.so", "mods/overlay/mod.c")
    if args.framework:
        build_mod("natartos_loader.so", "src/natartos_loader.c")
        build_mod("mod_template.so", "mods/template/mod.c")
    sources = list((ROOT / "mods/sd_apps").glob("*.[ch]"))
    sources += [ROOT / "examples/sd_apps/hello/hello.c", ROOT / "examples/sd_apps/hello/app.json",
                ROOT / "include/natartos_app.h", ROOT / "include/natartos_common.h",
                ROOT / "include/natartos_display.h", ROOT / "mods/overlay/sd_display.h"]
    artifacts = [out / "sd_app_runner", out / "apps/hello/bin/hello"]
    if not args.host_test:
        sources += [ROOT / "mods/menu/mod.c", ROOT / "mods/overlay/mod.c"]
        artifacts += [out / "mod_sd_apps.so", out / "hook_menu.so", out / "hook_overlay.so"]
    if args.framework:
        sources += [ROOT / "src/natartos_loader.c", ROOT / "mods/template/mod.c"]
        artifacts += [out / "natartos_loader.so", out / "mod_template.so"]
    info = {
        "upstream_base": BASE_REVISION, "compiler": compiler,
        "target": args.target if args.zig else "native-linux" if args.host_test else "aarch64-linux-gnu",
        "host_test": args.host_test,
        "sources": {str(s.relative_to(ROOT).as_posix()): hashlib.sha256(s.read_bytes()).hexdigest() for s in sources},
        "artifacts": {str(a.relative_to(out).as_posix()): hashlib.sha256(a.read_bytes()).hexdigest() for a in artifacts},
    }
    (out / "build-info.json").write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")
    print(f"Built in {out}; upstream baseline {BASE_REVISION}")


if __name__ == "__main__":
    main()
