# STiD135 shared-RF gain isolation

This is a second, bounded fix on top of the shared-RF initialization patch and
the matched TBS stack described in [LATEST-STACK.md](LATEST-STACK.md).
It applies in the kernel driver, independently of which DVB application tunes.

**Status: software regression tests pass; post-activation hardware continuity
validation is still required. Installing the module does not activate it in
an already running kernel. The operator decides when to reboot.**

## Problem and policy

`FE_STiD135_Algo()` previously called the ordinary LNF/IP3 gain-selection
routine during every acquisition. Its conditional write changes a shared RF
input's VGLNA gain by 6 dB, not just the requesting demodulator. There was no
check for other demodulators using the same RF input.

The new acquisition-only policy checks live hardware routing and reception,
plus explicit in-flight acquisitions, under the existing `master_lock`:

- If another same-RF demodulator is acquiring, has demodulator lock, or reports
  a found DVB-S/S2 carrier, retain the existing gain and continue the new tune.
  This also protects a found carrier during a temporary FEC/TS lock loss.
- With no protected same-RF sibling, retain the original hysteresis: low gain
  switches to LNF below AGC `0x3c00`; LNF switches to low gain above `0x8000`.
  The thresholds themselves do not trigger a change.
- Other RF inputs retain their gain bits and are independently eligible for
  gain changes. If hysteresis requests no change, sibling scanning is skipped.
- Failed gain, routing, lock-state or AGC reads prevent the gain write and
  fail the requesting tune. Unknown state is never treated as an idle RF.
  A checked byte read/write avoids the legacy field setter's ability to write
  after a failed read. Write errors also propagate.
- Acquisition reservations survive `FE_STiD135_GetDemodLock()` temporarily
  releasing the mutex, and are cleared on every return from the acquisition
  routine without clearing another demodulator's reservation. Duplicate
  simultaneous search of the same demodulator is rejected.

The change does not freeze normal AGC or signal telemetry. First-use tuner
initialization keeps the existing policy. It does not change the DVB userspace
ABI, SEC commands, RF power management, bridge/DMA code or firmware.

### Trade-offs and limits

Deferring gain selection can reduce sensitivity or overload headroom for the
new target. Gain is reconsidered on a later tune when no sibling is protected;
there is no background forced transition that could interrupt receivers.
A closed frontend whose hardware remains locked is conservatively protected.
Cached `demod_results[].locked` and retained initialization ownership alone
are deliberately not used to infer live reception.

Previous hardware tests reproduced sibling transport-stream continuity errors
with property-list retunes that sent no voltage, tone or DiSEqC commands. They
did **not** measure the gain register. Gain switching is therefore a plausible
mechanism, not a proven explanation of those errors. This patch does not claim
to resolve shared Unicable user-band collisions, coax voltage/command conflicts,
all TBS models, or the separate paired-demodulator wideband/high-symbol-rate
operations. Hardware validation must measure the outcome, not just lock status.

## Regression tests

Run from the checkout root:

```sh
python3 drivers/media/dvb-frontends/stid135/tests/shared_rf.py
python3 drivers/media/dvb-frontends/stid135/tests/shared_gain.py
CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -g' \
  python3 drivers/media/dvb-frontends/stid135/tests/shared_gain.py
```

The original 24 lifecycle cases remain. The 15 gain cases execute the actual
search/acquisition/lock-wait/gain code with register and signal-processing
stubs. See [coverage and limitations](tests/README.md#gain-isolation-tests).
Passing these tests does not simulate RF physics or prove loss-free playback.

## Compile against an already matched TBS stack

For a new installation, first use the [full-stack build guide](LATEST-STACK.md).
Do not mix this module with old headers or unmatched DVB/TBS module versions.
For a host already running the matched stack, this change can be built in
isolation using the preserved prepared build's headers and compatibility
configuration. No original build files need to be edited.

Clone this fork's `fix/stid135-shared-rf-init` branch into a fresh directory,
record its full commit with `git rev-parse HEAD`, and run both test suites.
`SOURCE` below is that checkout's `drivers/media/dvb-frontends/stid135`.
Set `MEDIA_BUILD` to the exact prepared tree used for the installed modules,
not a fresh generic upstream download. These are build-only commands:

```bash
set -euo pipefail
KVER="$(uname -r)"
KBUILD="/lib/modules/$KVER/build"
SOURCE=/path/to/reviewed/linux_media/drivers/media/dvb-frontends/stid135
MEDIA_BUILD=/path/to/matched/prepared/media_build
WORK="$(mktemp -d "$HOME/stid135-gain.XXXXXXXX")"
test -s "$KBUILD/Module.symvers"
mkdir "$WORK/candidate" "$WORK/linux"
cp -a "$SOURCE/." "$WORK/candidate/"
cp -a "$MEDIA_BUILD/linux/include" "$WORK/linux/"
cp -a "$MEDIA_BUILD/linux/kernel_version.h" "$WORK/linux/"
for h in compat.h config-compat.h config-mycompat.h; do
  cp -a "$MEDIA_BUILD/v4l/$h" "$WORK/candidate/"
done
cat > "$WORK/candidate/Kbuild" <<'KBUILD_EOF'
obj-m := stid135.o
stid135-y := stid135-fe.o chip.o stfe_utilities.o oxford_anafe_func.o stid135_init.o stid135_drv.o
NOSTDINC_FLAGS += -I$(src)/../linux/include -I$(src)/../linux/include/uapi
ccflags-y += -I$(src) -DHOST_PC -include $(src)/compat.h -g -Wno-format-truncation
KBUILD_EOF
nice -n 10 make -C "$KBUILD" M="$WORK/candidate" -j2 modules \
  2>&1 | tee "$WORK/build.log"
modinfo "$WORK/candidate/stid135.ko"
sha256sum "$WORK/candidate/stid135.ko"
```

This uses the kernel's [external module build interface](https://docs.kernel.org/kbuild/modules.html).
Treat any compiler, modpost or ABI error as a stop, not something to bypass.
Compare imports and the exported `stid135_attach` CRC to the installed module,
its real consumers and matching provider `Module.symvers`; `vermagic` alone is
insufficient. Newly imported kernel symbols also need verification. Use a
decoder supporting the actual kernel's symbol-version record format.
`dvb_attach` may bind the bridge dynamically through `__symbol_get`; such a
bridge has no direct `stid135_attach` import CRC. Verify the dynamic binding and
unchanged public export against the matching full-stack build rather than
mistaking an absent import record for an ABI check.

The native Linux 6.8.12/GCC 13 build passed 24 lifecycle and 15 gain cases,
including the gain suite with AddressSanitizer/UndefinedBehaviorSanitizer.
All 36 candidate imports matched their selected providers; the public export
CRC remained `0x5cdb5f5f`. These values describe this tested stack, not all
kernels. Its new STiD135 srcversion is `5DDB1F841FF4122877D8FDE`.

## Install without activation

After native compilation and ABI verification, sign a copy of the final module
with an existing trusted key using the target kernel's signing helper. Verify
both the signature and that its unsigned payload equals the built artifact.
Never publish the private key or modify/strip a module after signing. See the
[kernel signing documentation](https://docs.kernel.org/admin-guide/module-signing.html).

On a shared host, do not run broad upstream `make install`, reload helpers,
`rmmod`, service restarts or tuning tests during this installation. Resolve
the existing module with `modinfo -n stid135`. Retain a private backup of that
exact file and the `modules.*` indexes. The matched-stack transaction can stage
a complete copy of `updates/extra`, replace only its STiD135 file, verify that
every other module is byte-identical, then atomically exchange the directories
on the same filesystem. The exchanged old tree becomes the rollback copy.
Run `depmod` and verify the
selected file, hash, dependency resolution, signing metadata and all other
installed module hashes. If an index update fails, restore the old module and
saved indexes; do not rely on a second `depmod` to recover. Recovery must refuse
to overwrite intervening module-management changes.

Inspect the initramfs before installation. If it embeds `stid135`, include a
separately backed-up, verified image rebuild; otherwise leave the image alone.
Record boot ID, loaded module versions, service PIDs/start times and boot-image
hash before and after. Only the on-disk STiD135 module should change. Keep
machine-specific manifests, install/recovery scripts and credentials private.

## Operator-controlled activation and diagnostics

After the operator's maintenance reboot, verify:

```sh
uname -r
cat /sys/module/stid135/srcversion
modinfo -F srcversion stid135
cat /sys/module/stid135/parameters/shared_rf_debug
journalctl -k -b --no-pager | grep -Ei 'stid135|tbsecp3|unknown symbol|signature'
```

The loaded and installed STiD135 srcversions must match the new build record.
Before reboot they intentionally differ; the new parameter is not present in
the old loaded module. No reboot command is part of this procedure.

For a bounded, operator-approved post-activation test, enable diagnostics:

```sh
echo Y | sudo tee /sys/module/stid135/parameters/shared_rf_debug
# Run the agreed tuning/continuity test, then disable logging:
echo N | sudo tee /sys/module/stid135/parameters/shared_rf_debug
```

Each completed search can emit a rate-limited `shared-rf` line. The device
prefix identifies the I2C adapter/card; `demod` and `rf` are 1-based, whereas
`protected` uses bit 0 for demod 1. Fields include `phase=acquire`, `old`,
`requested`, AGC, protected mask, decision and search error. Decisions are
`unchecked`, `unchanged`, `deferred`, `changed` or `error`; `-1` means no gain
mode was read. This records the acquisition's gain decision when search
returns, not a precise timestamp of the register operation. Rate limiting may
omit lines during bursts. Default logging is off; there is no packet logging.

Repeat both direct no-SEC retunes and the normal Unicable workflow. Independently
capture sibling TS continuity counters, TEI/sync errors, packet counts and
overflows before/during/after multiple tunes. Verify target lock, service scan,
signal telemetry, concurrent acquisitions and separate RF inputs. Compare to
the saved baseline; a locked frontend or a `deferred` log alone is not success.

Rollback uses the retained exact old signed module and indexes while preserving
other changes. Restoring disk files likewise does not unload an already active
module; the operator controls the subsequent maintenance reboot.
