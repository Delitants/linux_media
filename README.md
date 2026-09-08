# TBS Linux media: STiD135 shared-RF fix

This fork's `fix/stid135-shared-rf-init` branch contains a targeted fix for
reinitializing an RF input that other STiD135 demodulators are already using.
It is based on the existing deployment's TBS driver revision, **not the latest
complete TBS media stack**.

- [Build, installation, rollback, and technical explanation](drivers/media/dvb-frontends/stid135/BUILDING-SHARED-RF.md)
- [Source-executing regression tests](drivers/media/dvb-frontends/stid135/tests/README.md)
- [Official TBS source](https://github.com/tbsdtv/linux_media)
- [Original upstream kernel README](README)

The guide distinguishes a module installed on disk from one loaded by the
kernel. Installation does not activate the fix in an already-loaded driver.
Keep reboot/module-reload decisions under the operator's control.
