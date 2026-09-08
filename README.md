# TBS Linux media: STiD135 shared-RF fix

This fork's `fix/stid135-shared-rf-init` branch contains a targeted fix for
reinitializing an RF input that other STiD135 demodulators are already using.
It includes official TBS `latest` revision `2ce787de` (2026-07-24), merged
without dropping the shared-RF fix. Upstream was checked on 2026-09-08 UTC.

- [Latest-stack build, installation, compatibility, and rollback](drivers/media/dvb-frontends/stid135/LATEST-STACK.md)
- [Fix explanation and historical single-module build](drivers/media/dvb-frontends/stid135/BUILDING-SHARED-RF.md)
- [Source-executing regression tests](drivers/media/dvb-frontends/stid135/tests/README.md)
- [Official TBS source](https://github.com/tbsdtv/linux_media)
- [Original upstream kernel README](README)

The guide distinguishes a module installed on disk from one loaded by the
kernel. Installation does not activate the fix in an already-loaded driver.
Keep reboot/module-reload decisions under the operator's control.
