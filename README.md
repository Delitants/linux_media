# TBS Linux media: STiD135 shared-RF isolation

This fork's `fix/stid135-shared-rf-init` branch contains two targeted fixes for
retuning one STiD135 demodulator without unnecessarily disturbing a shared RF
input used by its siblings:

1. **Shared initialization:** avoid repeating RF clock/FIFO and DiSEqC
   initialization while that input is already initialized.
2. **Shared gain isolation:** defer the optional 6 dB LNF/IP3 gain-mode change
   when another demodulator on that input is receiving or acquiring a signal.
   Exclusive-input automatic gain selection remains available. Read failures
   never authorize a gain write. Optional rate-limited diagnostics explain the
   decision.

**The gain patch still needs post-activation hardware continuity testing.**
It closes a specific source-level isolation gap, not a proven cure for every
retune-related packet loss. Shared-coax commands and high-symbol-rate paired
demodulator operations remain separate limitations.

It includes official TBS `latest` revision `2ce787de` (2026-07-24), merged
without dropping the shared-RF fix. Upstream was checked on 2026-09-08 UTC.

- [New gain fix: behavior, compilation, diagnostics, and validation](drivers/media/dvb-frontends/stid135/SHARED-RF-GAIN.md)
- [Latest-stack build, installation, compatibility, and rollback](drivers/media/dvb-frontends/stid135/LATEST-STACK.md)
- [Fix explanation and historical single-module build](drivers/media/dvb-frontends/stid135/BUILDING-SHARED-RF.md)
- [Source-executing regression tests](drivers/media/dvb-frontends/stid135/tests/README.md)
- [Official TBS source](https://github.com/tbsdtv/linux_media)
- [Original upstream kernel README](README)

The guide distinguishes a module installed on disk from one loaded by the
kernel. Installation does not activate the fix in an already-loaded driver.
Keep reboot/module-reload decisions under the operator's control.
