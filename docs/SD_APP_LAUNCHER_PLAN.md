# SD Card Application Launcher Plan

Status: original plan drafted 2026-11-15; implementation and SD/menu bench
results are described in the [runner guide](SD_APP_RUNNER.md) and
[2026-11-17 bench](SD_APP_BENCH_2026-11-17.md).

Extend Natartos so the owner can carry native applications on an SD card,
select them inside the goggles, and return reliably to the DJI interface.
The first version uses a phone and USB cable to activate Natartos after boot.
Automatic activation is a separate research track.

The first implementation provides the standalone runner and a console Hello
package. Both build for ARM64 and pass Linux integration checks. The first
[console bench test](SD_APP_BENCH_2026-11-16.md) also verified staging and
execution from an internal source package on the goggles. See the
[runner guide](SD_APP_RUNNER.md) for commands and current limits.

The second slice adds per-app More-menu entries, display hand-off for
`"display": true` apps, and graphical Hello. SD/menu launches and the display
guards were subsequently checked on the retail unit. Release installer and
phone-only installation integration remain follow-up work.

## First usable workflow

1. Power on the goggles, connect an Android phone, and use Toolya in Chrome
   to enable ADB and start Natartos.
2. Disconnect the cable and open **More** inside the goggles; each app on the
   card has its own entry.
3. Select an application. The launcher copies its package into internal
   storage, validates the staged copy, and starts it.
4. Exit the application and return to a responsive DJI interface.

Installation and saved files survive normal power cycles. Natartos activation
still repeats after each reboot in this first version. Phone compatibility
must be checked on the owner's Samsung; browser support alone is insufficient.

Begin with a **Hello Goggles** application that displays a message, accepts
the joystick and BACK button, and exits. A laptop may be used for development
and bench diagnostics; the completed field workflow must work without one.

## Build on the existing framework

| Component | Responsibility |
| --- | --- |
| Natartos loader and menu | Load the SD Apps mod; show one More-menu entry per app (limit raised from 8 to 32). |
| Natartos display support | Overlay shows the app's frames and restores the DJI display afterward. |
| SD Apps mod | Ask the runner for the card's apps at start-up and register their entries. |
| Standalone app runner | Validate, stage, supervise, hand over the display, and clean up. |
| Optional boot bootstrap | Activate Natartos automatically after a future, separate validation. |

Each app is an ordinary More-menu entry, registered like Doom and PC Stream,
rather than a list screen of our own. The eight-entry limit was only a
constant in the menu mod; it is now 32 and SD Apps uses at most 24. The
card is read once per `dji_glasses` start (see the runner guide's menu section).

The mod should perform little work inside `dji_glasses`. Copying files,
parsing manifests, and waiting for child processes belong in the standalone
runner. Start with only the menu, overlay, SD Apps, and any demonstrated
dependencies enabled.

Relevant interfaces are in the [menu API](../include/natartos_common.h),
[app registration example](../mods/doom/mod_menu.c),
[display hook](../mods/overlay/mod.c), and
[session start script](../deploy/inject.sh). Treat their current code as the
implementation reference where the prose specification has drifted.

Add build targets and release-manifest entries for the runner and mod;
the existing mod build loop alone only produces the mod library. Toolya's
default downloads target grom-labs releases, so they will not automatically
install this fork's additions. Use its existing local-manifest installation
path for initial development, then activate the installed build from the
phone. Pin the fork revision and distinguish activation from reinstalling
an upstream release.

## Application packages and internal storage

Version 1 reads ordinary directories on the external SD card. Archive
installation, an app store, and automatic application startup are outside
the first version.

```text
natartos/apps/hello/
  app.json
  bin/hello
  assets/
```

Proposed package manifest:

```json
{
  "schema": 1,
  "id": "hello",
  "name": "Hello Goggles",
  "version": "0.1.0",
  "entry": "bin/hello",
  "args": []
}
```

This `app.json` describes a runnable SD package. Natartos's mod descriptors
and release manifest retain their existing roles.

The runner must validate the actual executable as a compatible ARM64 ELF;
the manifest is not proof of compatibility. Reject unsupported interpreters
as well as the wrong architecture. Begin with a self-contained executable
and assets. Add packaged shared-library support after that path
works. Graphical apps must implement the display/input contract established
by the Hello Goggles example; arbitrary desktop applications are not assumed
to be compatible.

Resolve the external card from the mount table. The observed mount was
`/mnt/media_rw/sdcard0`; the internal media volume must not be mistaken for
the external card. Discover packages under its `natartos/apps` directory.
The card is mounted `noexec`, so execution happens from an internal copy.

Proposed internal paths:

| Path | Purpose |
| --- | --- |
| `/data/local/tmp/natartos-apps/packages/<id>/<digest>/` | Disposable, verified package cache. |
| `/data/natartos/appdata/<id>/` | Saved application data, retained across launches and cache cleanup. |
| `/data/local/tmp/natartos-apps/logs/` | Runner diagnostics and bounded application stdout/stderr. |

Confirm these paths and their SELinux labels during the compatibility step.
Do not assume that uid 0 or `chmod` alone makes execution possible. Make any
dependency on Natartos's existing session permission setup explicit; do not
add new firmware-specific kernel-memory patches to the runner.

Staging must stay within its dedicated cache. Check available space, copy
to an incomplete directory, verify the copied package, and rename it into
place only when complete. Reuse a cache entry only when its fingerprint
matches. Bound cache/log growth and remove only inactive entries owned by
this runner. Choose the storage budget after measuring the target device.

Reject absolute entry paths, traversal, links and special files, unsafe IDs,
malformed manifests, and unsupported executables. Treat titles as display
text. Pass arguments as an argv array; never interpolate SD-card metadata
into a root shell command. Hashes detect changes and incomplete transfers;
they do not make third-party code trustworthy. These are owner-selected,
privileged applications, not sandboxed untrusted apps.

## Launch and exit behavior

Run one application at a time, with its own working directory and process
group. Give it explicit package/data paths and a small documented environment.
Clear Natartos's inherited `LD_PRELOAD` before starting the child so UI hooks
and signal handlers are not accidentally injected into applications.

The runner owns the child's lifetime and exit status. Normal exit, launch
failure, a crash, or a tested exit gesture must release display/input use,
clear only the runner's transient flags, and restore the DJI interface.
Use a bounded termination sequence for a hung child and target only its
process group. Saved data must remain intact.

Reuse the overlay's handoff where suitable, but verify the complete return
path. Current crash-guard and safe-mode claims need checking against actual
behavior before relying on them. A process merely remaining alive is not
evidence that the display is healthy. Keep launcher experiments on the bench
until restoration of the normal view and controls is reliable.

## Implementation milestones

| Milestone | Deliverable and completion condition |
| --- | --- |
| 1. Build and compatibility baseline | Pin the upstream base and an ARM64 toolchain; build the minimal mod and Hello executable. Record the actual firmware, storage, and execution context. Verify phone-assisted Natartos activation. |
| 2. Package runner | Implement discovery, manifest validation, staging and direct execution. A Hello package on SD runs from internal storage and produces a captured success result. |
| 3. Goggles interface | Add per-app More-menu entries and the graphical Hello example. Launch and exit work after disconnecting USB. Document the small app display/input contract. *(Implemented and host-tested; device check pending.)* |
| 4. Failure handling | Handle removal/full storage, bad packages, crash/hang, cancellation and repeated launches. Fix framework recovery behavior required by these cases. |
| 5. Field workflow | Repeat cold boot → phone activation → cable removal → app → DJI interface. Establish a reliable offline activation route before promising operation without mobile data. |

The first coding change should implement the package reader/validator and
runner with a Hello fixture. Give them configurable source/cache roots for
host testing. Then prove staging and execution on the goggles before adding
the menu entries. This keeps the first hardware experiment small.

Suggested implementation locations are `mods/sd_apps/` for the mod,
runner and manifest code, and `examples/sd_apps/hello/` for the package example.
Keep changes to the shared loader, menu, and overlay limited to interfaces
or recovery fixes the new feature actually needs.

## Validation and device changes

Host checks should cover valid and malformed packages, incompatible ELF
files, escaping paths/links, arguments containing shell characters, interrupted
staging, cache reuse, and cleanup boundaries. Device checks should cover:

- Missing/removable SD card and insufficient internal storage without
  blocking the DJI menu or running an incomplete copy.
- Successful launch, ordinary exit, app failure, and forced exit with saved
  data retained and a responsive DJI view afterward.
- Repeated launch/exit cycles, with bounded cache/log usage and no stray
  children or leaked input/display ownership.
- Phone activation after a cold boot, followed by cable-free app use.
- Recovery to the normal video view using a bench video source or grounded
  air unit before considering flight use.

The code snapshot used for initial investigation was build 35808. Recheck
the actual device after any update and keep matching clean backups. Document
the exact install paths, build checks, and restore steps before the first
device installation.

Application staging belongs under `/data`. Framework installation may use
the existing, explicitly reviewed Natartos directory under `/vendor`.
Version 1 adds no writes to `/system`, boot settings, calibration, or stock
vendor libraries, and does not use `noverity`.

## Phone activation and later standalone startup

Use existing Toolya activation for the first bench proof. Check Chrome USB
permission and device re-enumeration on the owner's phone. Then test the
startup workflow without mobile data. If the current site cannot operate
offline, a small offline-capable phone helper is a separate integration task;
the SD launcher should remain independent of its implementation.

Automatic startup is optional for version 1. The observed GPS HAL loading
path under `/vendor` is a research candidate, not a validated bootstrap.
It runs inside a core DJI service and requires firmware-specific review,
preservation of stock behavior, and a marker-only bench test before any
application activation. Keep that work separate from SD launching so a
useful phone-assisted version can ship first.
