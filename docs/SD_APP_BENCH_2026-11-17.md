# Matched display runner/overlay update — 2026-11-17

The owner requested installation of the display-readiness and stale-ownership
fixes and confirmed a charged battery and availability for immediate UI checks.
This session updated an existing Natartos installation on the retail unit.
It did not install a boot/persistence mechanism.

## Baseline and installation

- Retail build 35808; ADB root context `u:r:su:s0`.
- The owner started Natartos before the update. SELinux read `0` (permissive);
  this session did not change its state.
- `/system` remained read-only on `dm-0`. `/vendor` was initially read-only on
  its direct ext4 mount.
- The existing SD Hello package was discovered as `hello`, version `0.2.0`.
  No SD package contents were changed by the update.
- The existing overlay and both installed runner copies were backed up locally
  and under a dedicated `/data/natartos/backups/` directory. Each backup was
  checked against the source file's device SHA-256 before replacement.
- Incoming artifacts were checked against the production build record before
  transfer, after staging, and after publication. Files were copied to sibling
  temporary names and renamed over their destinations, preserving the old
  mapped overlay inode until the UI restarted.

| Installed artifact | Location | SHA-256 |
| --- | --- | --- |
| Runner | `/data/natartos/bin/sd_app_runner` and `/data/local/tmp/sd_app_runner` | `3865c46064c5b4e7132fabc0a4789b054e1cb0186ee4faadfe1e2c3121ec3f2d` |
| Overlay | `/vendor/natartos/mods/hook_overlay.so` | `969d3e63111155ac8aabfa6bfa71bd62d93c19fe8aef6a373b0a415ebd407770` |

Only `/vendor` was remounted writable for the overlay replacement. The first
attempt to remount it read-only returned `Device or resource busy`. After
restarting Natartos with the existing `inject.sh`, the read-only remount
succeeded. Both protected mounts were then rechecked, along with all three
installed artifact hashes and the fresh UI process's overlay mapping.

The loader, menu mod, enabled-mod markers, injection script and installed
release manifest were retained. The release manifest predates these bench
overrides; the private deployment record carries the actual before/after hashes.

## Device checks

| Check | Evidence/result |
| --- | --- |
| Fresh Natartos UI | Owner confirmed normal view and More menu were responsive after restart. |
| Missing overlay | The new runner read the real SD Hello package but used an isolated empty display directory and separate cache/data roots. It returned 125, `overlay did not acknowledge`, before Hello wrote a launch record. Temporary display flags were removed. |
| Hello from More | Owner launched Hello through its menu entry and reported it worked. The app log identified `/mnt/media_rw/sdcard0/natartos/apps/hello`, recorded six centre presses, and ended `Hello Goggles completed.` The overlay logged `repaint: woke=1` and `clean in-process exit OK`; the UI process stayed running. |
| Runner death | A separate bounded Hello run published a frame with acknowledgement `1`. Its recorded runner PID and command line were checked, then that runner alone received SIGKILL. The overlay logged `SD runner exited` and removed the display files. No Hello process remained. |
| UI after runner death | Repaint logged `woke=0` and used the existing UI-restart fallback. The owner immediately confirmed the normal display and controls returned. This was recovery through restart, not a clean in-process repaint. |
| Normal launch after recovery | Owner launched Hello again through More and confirmed joystick, centre press, BACK exit and responsive menus. The final app log again identified the SD source and ended successfully. The display files were gone and no Hello process remained. |

The directly started Hello had a 60-second app timeout. The intentional kill
occurred shortly after its acknowledged frame, rather than waiting for timeout.
The persistent `fb_owner.lock` and `fb_overlay.lock` files are retained by
design; their presence alone is not evidence of active ownership.

## Limits and evidence

The missing-overlay guard and runner-death detection were exercised on the
device. Simultaneous runner/overlay death and overlay death alone were tested
in the 76-check x86-64/ARM64 host suites, not deliberately induced here. The
DJI repaint fallback remains relevant when starting an app outside the menu.

Private command logs, original binaries, before/after app and overlay logs,
hash records and deployment metadata stay in the ignored research capture
directory. These backups are copies of the replaced application artifacts;
they do not establish partition or firmware recovery.
