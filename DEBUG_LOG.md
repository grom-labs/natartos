# Debug log

## 2026-11-18 — Remove input dependency from unattended boot

- **Observation:** the first enforcing cold boot produced a fresh GPS proxy
  marker and a normal display, but the qualifier logged `startup bypass`
  before its read-only permission probe.
- **Root cause:** `src/boot_activate/supervisor.c:303` at `80993d9` made
  `key_bypass()` a mandatory precondition for the probe. That function at line
  103 combined input-access failures and BACK state into the same result; the
  record did not establish that a button was held. Input state was an unwanted
  dependency for unattended qualification and activation.
- **Fix:** remove input-device checks from both modes; retain persistent
  enable/disable files, profile checks and automatic failure handling. Route
  profile failures to the bounded boot log for diagnosis.
- **Class:** unnecessary-startup-dependency
- **Recently-touched?** yes
- **Validation:** 32 supervision/handoff scenarios and the profile matrix passed
  on ARM64 and x86-64. Both boot binaries pass an audit excluding input-device
  paths. The corrected read-only helper passed live warm and enforcing cold
  qualification, reading the matching state word with zero memory writes and
  zero UI changes. Stock GPS was restored and the opt-in flag removed afterward.

## 2026-11-18 — Read initial boot health settings without libc availability

- **Observation:** the real GUI loaded the overlay and reached its frame hook,
  but the supervisor timed out and restored stock; matching stock Android
  environment values did not resolve it. Diagnostics reported health mode zero.
- **Root cause:** `src/boot_activate/loader_health.h:11` at `6792ff5` interpreted
  unavailable libc getenv values as a manual launch. The supervisor's health
  environment was consequently never used; the precise libc lookup failure
  was not established.
- **Fix:** use a bounded raw read of the initial environment when libc provides
  neither field, reject duplicates/truncation, and reset health only after a
  complete 24-byte report. Retain diagnostic mode/write results.
- **Class:** unavailable-runtime-dependency
- **Recently-touched?** yes
- **Validation:** real warm session acknowledged the render signal; menu controls
  responded; deliberately killing the supervisor restored stock without mods.
  Linux supervision/handoff and Android loader fixtures pass with unavailable
  getenv. See the dated boot activation bench record.
