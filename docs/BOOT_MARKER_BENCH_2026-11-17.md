# Marker proxy service test — 2026-11-17

The production marker proxy executed in the stock GPS consumer on a retail
build 35808 device during a controlled service restart and a subsequent full
power cycle. The cold-boot marker recorded `stock=ready` in `dji_services` with
**SELinux enforcing** before Natartos was started. The original GPS library
was restored after both phases. This validates the marker execution point;
it does not implement unattended Natartos activation or establish a position fix.

Candidate source: `9b34102` (bench instructions clarified in `358e2a2`).
ARM64 candidate SHA-256:
`014fb8934b9d1ff229d341af48c9f1d721ef7115b1f508a3ec1730dd71d540bd`.
The production proxy's source and bytes were not modified for service loading.

## Baseline and preparation

- The owner had already enabled root ADB and started Natartos. The root shell
  was `su`; `/sys/fs/selinux/enforce` was `0` throughout this test.
- `/system` was read-only through `dm-0`; `/vendor` was read-only on its direct
  ext4 mount. The display indicated 92% battery with USB connected.
- The bridge captured the normal display and exercised menu open, selection
  movement and BACK. No aircraft was connected.
- The stock `dji_sys` process had context `u:r:dji_services:s0`, effective
  capabilities `0000003fffffffff`, and the original GPS library mapped.
- Its GNSS task repeatedly reported status `1` and zero satellites. No position
  fix was established. The inspected init/start return-code messages were not
  present in the collected live log, so those codes could not be compared.

The original GPS library was copied to the host with binary-safe ADB output
and verified against device SHA-256 and the previously captured firmware.
An unused stock copy was prepared at `/vendor/lib64/gps.default.stock.so` with
the original bytes, ownership/mode and `vendor_file` label. The existing GPS
path was initially unchanged. The production marker was pre-created as root,
mode `0600`, with the `system_data_file` label.

## Separate-process forwarding check

A private path-only variant of the proxy was loaded by an isolated helper
against the real original GPS library. The helper called the original and
forwarded getters with a null device argument and verified identical non-null
interface pointers. It did not call the GPS initialization/start callbacks.
The constructor recorded `stock=ready` in the helper's `su` context.

This path variant was not used for the subsequent service test. The public
production candidate retained its documented paths and SHA-256.

## Timed service test and restoration

File bind mounts were rejected with `EINVAL`, both at the GPS path and between
two noncritical scratch files. No service was restarted for the rejected bind
attempt. The planned transient-mount route was therefore not available.

A stock restoration drill verified copying the saved original to a sibling
file, checking its hash/label, atomically renaming it over the original path
and returning `/vendor` to read-only. Releasing the running service's mapping
was necessary before the read-only remount would succeed.

For the live test, an independently detached shell timer was armed before
publishing the proxy. After 60 seconds it restored the known stock bytes and
restarted the stock service. This is an observed restoration in the current
root/permissive session, not a tested recovery method for a failed cold boot.

The proxy was copied to a sibling file, hash-checked, labeled `vendor_file` and
atomically published. The old service was stopped, `/vendor` remounted read-only,
and the service started again. USB temporarily disconnected and then returned.

The running service mapped both the production proxy and the preserved stock
library. Its marker recorded:

```text
uid=0 euid=0 gid=0 egid=0 stock=ready context=u:r:dji_services:s0 CapEff=0000003fffffffff enforcing=0
```

The boot ID did not change. This identifies service-start execution in the
existing boot. The GNSS task continued reporting the same status `1` and zero
satellites during the proxy interval, and after restoration. That is useful
continuity evidence, not proof of satellite reception or full GNSS behavior.

The timer subsequently restored the original hash, restarted `dji_sys`, and
returned `/vendor` to read-only. The stock library alone was mapped afterward.
No SELinux toggle, physical-memory access, bootloader, partition-image write
or security-policy change was performed by the bench commands.

## Display and final state

The existing UI process survived the service restarts, but its captures stopped
repainting reliably. The installed Natartos injection script was run again to
refresh that session, without changing its artifacts or SELinux state. An early
capture raced display startup and reported no framebuffer; a subsequent capture
showed the normal HUD. Menu open, movement to Album and BACK were then observed
through the bridge. This was recovery through a UI restart, not proof that a
warm restart of `dji_sys` is transparent to the existing UI.

At completion of this phase:

- The original GPS library was active, with matching stock SHA-256.
- `/system` and `/vendor` were read-only; `dji_sys` was running.
- SELinux remained at its pre-test value `0`; Natartos was running again.
- The unused stock copy, marker and private staged backups were retained.
  No proxy was left active at the boot-loading path.

Cold boot and enforcing-mode operation were still pending at the end of that
first phase. The follow-up below records their separate validation. Private
command transcripts, raw logs, unit-specific identifiers, marker boot IDs,
original binaries and screenshots remain outside this source repository.

## Cold-boot follow-up

The owner authorized the cold-boot test and was available for the power cycle
and Toolya steps. The same production proxy was installed after rechecking its
source/artifact hashes, the stock backup, ownership and labels. `/vendor` was
returned to read-only before the handoff. A five-minute detached stock-restore
timer protected a delayed handoff in the existing session only; powering off
kills that timer. It was not a recovery mechanism for the next boot.

The owner powered the goggles off and back on, then completed Toolya's
**Enable ADB → Root** steps while leaving **Natartos stopped**. The owner
reported a normal boot. The host observed a different boot ID and exactly one
new record for that boot:

```text
uid=0 euid=0 gid=0 egid=0 stock=ready context=u:r:dji_services:s0 CapEff=0000003fffffffff enforcing=1
```

The marker's boot ID matched the live new boot, not either earlier warm-service
record. The proxy and stock-copy hashes still matched their expected bytes,
and `init.svc.dji_sys` was `running`. This establishes that the proxy resolved
the original getter and wrote its marker during a cold boot under enforcement.

The post-entry `su` shell was denied both reading `/sys/fs/selinux/enforce`
and running `getenforce`. The affirmative runtime measurement above came
from the probe in `dji_services`, not from interpreting a denied shell read.
The boot command line also declared enforcing SELinux and enforcing verity,
which is corroboration rather than a substitute for the marker's measurement.

Available GNSS logs continued reporting status `1`; a report with `num_svs 1`
was captured before Natartos startup. This supports continued status reporting,
not a verified position fix or complete GNSS regression test. Init/start return
codes still were not available in the collected live log.

After the enforcing evidence was saved, the host ADB server was released so
the owner could start Natartos in Toolya. On reconnection the host observed
Natartos mapped and runtime enforcement `0`, consistent with the existing
Natartos startup workflow. The bench scripts did not execute a permission
toggle or physical-memory command themselves.

The detached restoration script then republished the verified original library,
restarted the stock service and returned `/vendor` to read-only. The existing
Natartos UI was refreshed again after that service restart. Through the bridge,
the host observed the normal HUD, menu opening, two DOWN taps selecting
Transmission, and BACK closing the menu.

Final checks found:

- Both GPS paths held the original stock hash; only the original boot-loading
  path was active. The unused stock-copy file and marker were retained.
- `dji_sys` was running; the original GPS library was mapped again.
- `/system` remained read-only on `dm-0`, and `/vendor` was read-only.
- Natartos was running in the owner's ordinary permissive session after cleanup.

This is one successful cold-boot marker test on build 35808, followed by a
verified restoration in a working root/permissive session. It is not proof of
recovery from an unbootable device, compatibility with another firmware build,
unattended launcher startup, satellite navigation performance or flight safety.
