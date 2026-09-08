# STiD135 shared-RF fix: build and installation guide

**Historical procedure:** this reproduces the original May 2025-based fix at
`78d2660`. The branch now includes newer upstream code. Use the
[latest-stack guide](LATEST-STACK.md) for that upgrade; do not mix its newer
DVB definitions with this old single-module/header recipe.

This guide covers a **targeted replacement of `stid135.ko` on an existing,
working TBS installation**. It does not install an entire media stack or
upgrade Linux. Keep a recovery console and backups before changing drivers.

**Do not unload/reload DVB modules or reboot a streaming host as part of the
build/install steps.** The new file is for the next operator-approved boot.
Loading a replacement while sibling adapters are active is not safe.

## 1. Version of this historical build

The original build was not latest. It preserved the deployed source/header baseline.
The following upstream comparison was checked on **2026-09-08 UTC**:

| Component | Revision | Meaning |
| --- | --- | --- |
| Original TBS `linux_media` base | [`b00b2678fc0a23cf4956f33a84d50bde1d549fea`](https://github.com/tbsdtv/linux_media/commit/b00b2678fc0a23cf4956f33a84d50bde1d549fea) | 2025-05-25 UTC (May 26 in the commit's +03:00 timezone); matches the existing installation |
| Fixed driver source | [`78d26607153340717b1672da122b38f6c0654df2`](https://github.com/Delitants/linux_media/commit/78d26607153340717b1672da122b38f6c0654df2) | Base plus the shared-RF fix and its regression tests |
| Official TBS `linux_media/latest` at check time | [`2ce787de9f6e81b4294692a45de15596ae2e1322`](https://github.com/tbsdtv/linux_media/commit/2ce787de9f6e81b4294692a45de15596ae2e1322) | 2026-07-24; 16 upstream commits after our base |
| Existing prepared `media_build` baseline | [`bc02baf59046b02e3eb71653d8aa8d98e79dc4e1`](https://github.com/tbsdtv/media_build/commit/bc02baf59046b02e3eb71653d8aa8d98e79dc4e1) | Includes locally prepared compatibility headers; retain local build adjustments |
| Official `media_build` HEAD at check time | [`ff326ab69e786d447a0e0e967cb45acb5d5ca9ff`](https://github.com/tbsdtv/media_build/commit/ff326ab69e786d447a0e0e967cb45acb5d5ca9ff) | 2025-07-06; not substituted into this build |

The [upstream comparison](https://github.com/tbsdtv/linux_media/compare/b00b2678fc0a23cf4956f33a84d50bde1d549fea...2ce787de9f6e81b4294692a45de15596ae2e1322)
includes newer board support, bridge changes, and FEC/APSK enumeration changes.
The STiD135 frontend changes there rename APSK constants; they do not contain
this shared-RF ownership fix. Mixing the newer DVB headers into the old media
stack is not an equivalent or verified upgrade.

The fix branch can acquire documentation commits without changing the driver.
Pin the **full code revision** above for reproducible builds. Check the live
[official branch](https://github.com/tbsdtv/linux_media/commits/latest/) before
calling any future revision "latest". A rebase onto newer upstream code needs
separate build, ABI, userspace-enumeration, and hardware validation.

## 2. What the fix does

An STiD135 chip exposes eight demodulators and four RF inputs. Several Linux
frontends can share one physical RF input, including single-input/Unicable
configurations. Frontend numbers are not independent RF hardware.

Previously, each `stid135_init()` in the non-multiswitch path called
`fe_stid135_tuner_enable()` even when a sibling was already using that RF.
The enable routine toggles the shared `STOPCLK_STOP_CKTUNER` clock/FIFO reset
with 10 ms and 100 ms waits. Repeating that initialization can interrupt the
other demodulators sharing the input. Disabling `dvb_powerdown_on_sleep` alone
does not prevent this initialization-time reset.

The fix changes `stid135-fe.c`, not the low-level tuner reset sequence:

1. Track initialized users and hardware readiness separately for each RF.
   Track whether each frontend already owns a user reference.
2. Keep normal tuner/DiSEqC initialization and reset on first use. Once ready,
   only route the new demodulator to the RF; do not reset its siblings' input.
3. Make repeated init, sleep, and RF ownership drops idempotent. The one-shot
   release callback handles active or already-inactive ownership. Sleeping or
   releasing one frontend cannot power down an RF still owned by another.
4. Invalidate readiness whenever standby is attempted, even after partial
   failure. If a board callback suppresses standby, retain readiness because
   the hardware was not powered down. Failed init acquires no new ownership
   and only cleans up an unowned RF.
5. Serialize these state changes and runtime RF operations with the existing
   `status_lock`. Clear a previous **software** chip-error latch before retrying
   init; this is not a hardware reset. New I/O failures still propagate.

The existing multiswitch (`mode=0`) probe behavior is retained. Ownership
counts successful frontend initialization, not open file descriptors. If the
kernel suppresses the sleep callback, ownership can persist until release.

### What this does not fix

- It applies to the **STiD135 driver**, not every TBS model or every possible
  cause of channel interruptions.
- It does not arbitrate voltage, tone, DiSEqC, or Unicable commands between
  applications. Shared-coax command collisions or duplicate user-band slots
  can still disrupt other receivers.
- It does not change EN50494/EN50607 encoding, user-band numbering, RF mapping,
  firmware, tuning parameters, or softcam behavior.
- It cannot detect arbitrary external power/reset changes. Existing parent
  driver attach/lifetime serialization is assumed; the global base list is
  not redesigned.
- Source-level tests are not proof of uninterrupted reception. That requires
  simultaneous transport-stream monitoring **after loading the fixed module**.

## 3. Requirements and preflight

Use a Bash shell on the target Linux architecture. The build examples use
GCC, GNU make, Python 3, Git, binutils, and kmod utilities. Debian/Ubuntu's
usual package names are `build-essential`, `python3`, `git`, `binutils`, `kmod`,
`libelf-dev`, `libssl-dev`, `bc`, `bison`, and `flex`. Install matching kernel
headers/build files using the distribution's normal procedure. A custom
kernel may have no matching `linux-headers-$(uname -r)` package.

You need all of the following before proceeding:

- `/lib/modules/<target-kernel>/build` is prepared for that **exact** kernel,
  configuration, architecture, and compatible compiler. With symbol versioning,
  retain the real kernel's `Module.symvers`; `modules_prepare` alone is not
  sufficient. See [external-module requirements](https://docs.kernel.org/kbuild/modules.html).
- The existing TBS source and the **prepared build tree used for the installed
  media stack**, including generated compatibility headers and local patches.
  A fresh clone of `media_build` alone does not provide those generated files.
- Sufficient free space, a backup destination, and no concurrent kernel,
  driver, package, or `depmod` changes during installation/rollback.
- Any required module-signing key is already trusted by the target kernel.
  Do not disable signature enforcement to work around a signing failure.

Record the current state; these commands do not change the running driver:

```bash
set -euo pipefail
KVER="$(uname -r)"                         # Target this kernel, not an upgrade.
KBUILD="/lib/modules/$KVER/build"
MODROOT="$(readlink -f "/lib/modules/$KVER")"
MODFILE="$(readlink -f "$(modinfo -k "$KVER" -n stid135)")"
BOOT_BEFORE="$(cat /proc/sys/kernel/random/boot_id)"
LOADED_BEFORE="$(cat /sys/module/stid135/srcversion)"
test -d "$KBUILD"
test -s "$KBUILD/Module.symvers"
test -f "$MODFILE"
case "$MODFILE" in
  "$MODROOT"/*.ko) ;;
  *) echo 'Stop: this recipe requires an existing uncompressed .ko.' >&2; exit 1 ;;
esac
uname -a
modinfo "$MODFILE"
printf 'Loaded srcversion: %s\n' "$LOADED_BEFORE"
sha256sum "$MODFILE"
```

The examples assume `stid135` is currently loaded, as on the tested host.
Stop if it is absent; do not load it just to satisfy these preflight commands.
For compressed modules (`.ko.xz`, `.ko.zst`, `.ko.gz`), built-in drivers, or
other architectures, adapt and review the packaging/build procedure before
installing anything. Resolved paths accommodate merged `/usr` layouts.

## 4. Obtain the exact source and run tests

This creates a new sparse checkout; it does not change the installed source
tree. Use a new directory name and build paths without spaces.

```bash
CODE_REV=78d26607153340717b1672da122b38f6c0654df2
git clone --filter=blob:none --depth 1 --sparse --single-branch \
  --branch fix/stid135-shared-rf-init \
  https://github.com/Delitants/linux_media.git linux_media-stid135-fix
cd linux_media-stid135-fix
git sparse-checkout set drivers/media/dvb-frontends/stid135
git fetch --depth 1 --filter=blob:none origin "$CODE_REV"
git checkout --detach "$CODE_REV"
test "$(git rev-parse HEAD)" = "$CODE_REV"
test -z "$(git status --porcelain --untracked-files=all)"
SOURCE="$PWD/drivers/media/dvb-frontends/stid135"
python3 "$SOURCE/tests/shared_rf.py"
```

Expected result: **24 passing cases**. Tests execute the actual source callback
bodies and selected low-level helpers with checked hardware stubs and pthread
contention. They do not open DVB devices or tune any adapter.

Optional sanitizer commands and coverage details are in the
[test README](tests/README.md). Do not proceed with a failed test or a source
tree you cannot identify.

## 5. Compile only `stid135.ko`

Continue in the same shell. Set `MEDIA_BUILD` to your **existing prepared**
tree, not a new download. Keep all its local compatibility adjustments.

```bash
MEDIA_BUILD=/path/to/existing/prepared/media_build  # Change this path.
test -d "$MEDIA_BUILD/linux/include"
test -s "$MEDIA_BUILD/linux/kernel_version.h"
for header in compat.h config-compat.h config-mycompat.h; do
  test -f "$MEDIA_BUILD/v4l/$header"
done

STAGE="$(mktemp -d "$HOME/stid135-build.XXXXXXXX")"
mkdir -p "$STAGE/candidate" "$STAGE/linux"
cp -a "$SOURCE/." "$STAGE/candidate/"
cp -a "$MEDIA_BUILD/linux/include" "$STAGE/linux/include"
cp -a "$MEDIA_BUILD/linux/kernel_version.h" "$STAGE/linux/"
for header in compat.h config-compat.h config-mycompat.h; do
  cp -a "$MEDIA_BUILD/v4l/$header" "$STAGE/candidate/"
done
cat > "$STAGE/candidate/Kbuild" <<'KBUILD_EOF'
obj-m := stid135.o
stid135-y := stid135-fe.o chip.o stfe_utilities.o oxford_anafe_func.o stid135_init.o stid135_drv.o
NOSTDINC_FLAGS += -I$(src)/../linux/include -I$(src)/../linux/include/uapi
ccflags-y += -I$(src) -DHOST_PC -include $(src)/compat.h -g -Wno-format-truncation
KBUILD_EOF

make -C "$KBUILD" M="$STAGE/candidate" -j2 modules 2>&1 | tee "$STAGE/build.log"
CANDIDATE="$STAGE/candidate/stid135.ko"
test -s "$CANDIDATE"
modinfo "$CANDIDATE"
sha256sum "$CANDIDATE"
test "$(modinfo -F vermagic "$CANDIDATE")" = "$(modinfo -F vermagic "$MODFILE")"
printf 'Build artifacts: %s\n' "$STAGE"
```

This is the isolated Kbuild recipe used for the validated Linux 6.8.12/GCC 13
build. The original source, prepared `media_build`, and unrelated modules are
not modified. It is not a promise of compatibility with arbitrary kernels.

**ABI review gate:** matching `vermagic` is necessary, not sufficient. Confirm
the imported symbol versions against the target kernel and the exported
`stid135_attach` CRC against the installed stack. A clean build of the
unmodified base with the same copied headers provides a comparison artifact.
Use module/ELF tooling that can actually decode the target kernel's version
records; never interpret empty output or a parsing error as an ABI match.
The tested custom kernel needed an ELF-level check because its kmod tool did
not decode its compact symbol-version records. That check matched 34 imports
and the original `stid135_attach` export CRC (`0xa029cee1`). These values are
evidence for that build, not universal constants for other kernels.

Do not bypass compiler, modpost, or ABI errors. In particular, undefined APSK
names can indicate mismatched media source and headers, not missing arbitrary
`#define` statements.

Avoid the broad `media_build` `make install`, `rminstall`, `reload`, and `rmmod`
targets on a shared production host. They are not the targeted procedure here.
Inspect a build tree's default target before using it; it can include helper
hooks beyond compilation. For a fresh full-stack installation, prepare and
validate a compatible stack separately using the
[official build project](https://github.com/tbsdtv/media_build), preferably on
a spare host. This guide does not validate a fresh clone or a full-stack upgrade.

## 6. Sign the final artifact if required

Keep an unsigned build copy. If your kernel requires signatures, sign a copy
with an **already trusted** key/certificate and the target kernel's signing
tool. Replace the two placeholder paths; never put private keys in Git.

```bash
TO_INSTALL="$STAGE/stid135-install.ko"
cp -a "$CANDIDATE" "$TO_INSTALL"
# Run the signing lines only with your actual, trusted credentials:
SIGN_KEY=/path/to/enrolled-key.priv
SIGN_CERT=/path/to/certificate.x509
"$KBUILD/scripts/sign-file" sha256 "$SIGN_KEY" "$SIGN_CERT" "$TO_INSTALL"
modinfo -F signer "$TO_INSTALL"
sha256sum "$TO_INSTALL"
```

If signing is not required, copy the candidate to `TO_INSTALL` but omit the
signing lines. A displayed signer name does not prove that the target kernel
trusts the certificate. Never strip or modify the module after signing. See
the [kernel signing documentation](https://docs.kernel.org/admin-guide/module-signing.html).

## 7. Back up and install on disk, without activation

Proceed only after the source, tests, build, ABI, and signing gates pass.
Use the same Bash shell/variables. Root users may omit `sudo`. The example
backs up the exact existing module plus the top-level `modules.*` metadata
before replacing anything. Keep that backup outside the build's disposable
directory. Do not run another package/driver installer at the same time.

```bash
set -euo pipefail
BACKUP="$(sudo mktemp -d /var/backups/stid135-before-fix.XXXXXXXX)"
sudo chmod 700 "$BACKUP"
sudo mkdir "$BACKUP/module-metadata"
sudo cp -a "$MODFILE" "$BACKUP/stid135.ko"
sudo cmp -- "$MODFILE" "$BACKUP/stid135.ko"
find "$MODROOT" -maxdepth 1 -type f -name 'modules.*' -print0 \
  > "$STAGE/module-metadata-files.list"
test -s "$STAGE/module-metadata-files.list"
while IFS= read -r -d '' metadata; do
  sudo cp -a -- "$metadata" "$BACKUP/module-metadata/"
  sudo cmp -- "$metadata" "$BACKUP/module-metadata/${metadata##*/}"
done < "$STAGE/module-metadata-files.list"
printf '%s\n' "$MODFILE" | sudo tee "$BACKUP/module-path.txt" >/dev/null
printf '%s\n' "$KVER" | sudo tee "$BACKUP/kernel.txt" >/dev/null
sudo sha256sum "$BACKUP/stid135.ko"
printf 'Retain backup directory: %s\n' "$BACKUP"

# Same-directory rename avoids exposing a partially copied module file.
NEWFILE="$(sudo mktemp "${MODFILE}.new.XXXXXXXX")"
sudo install -o root -g root -m 0644 "$TO_INSTALL" "$NEWFILE"
sudo cmp -- "$TO_INSTALL" "$NEWFILE"
sudo mv -fT -- "$NEWFILE" "$MODFILE"
if ! sudo depmod -a "$KVER"; then
  echo 'depmod failed: restore the backup before any reboot; see rollback.' >&2
  exit 1
fi
test "$(readlink -f "$(modinfo -k "$KVER" -n stid135)")" = "$MODFILE"
test "$(modinfo -F srcversion "$MODFILE")" = "$(modinfo -F srcversion "$TO_INSTALL")"
sudo cmp -- "$TO_INSTALL" "$MODFILE"
test "$(cat /proc/sys/kernel/random/boot_id)" = "$BOOT_BEFORE"
test "$(cat /sys/module/stid135/srcversion)" = "$LOADED_BEFORE"
printf 'Installed on disk; loaded srcversion is still %s\n' "$LOADED_BEFORE"
```

`depmod` updates dependency indexes; it does **not** reload the running module.
These checks verify that the loaded source version and boot ID did not change.
They are not a general audit of other processes on the machine.

### Check the boot image

Check whether the boot image for **this kernel** contains a separate copy of
the module. On Debian/Ubuntu systems with `initramfs-tools`:

```bash
INITRD="/boot/initrd.img-$KVER"
sudo lsinitramfs "$INITRD" > "$STAGE/initramfs-files.txt"
grep -E '(^|/)stid135\.ko(\.(xz|zst|gz))?$' "$STAGE/initramfs-files.txt" || true
```

Inspect the output and listing command's exit status. A module-name entry in
a modprobe configuration is not a copy of `stid135.ko`. If a copy is present,
back up the image to `BACKUP`, then rebuild **only that kernel's image** using
your distribution's tool (for example, `sudo update-initramfs -u -k "$KVER"`
on `initramfs-tools` systems). Validate that the rebuilt image contains the
new module before considering installation complete. Do not reboot if that
step fails; restore the saved image and module. Dracut systems need their
own image inspection/rebuild procedure, not the Debian command.

The tested host had no embedded `stid135.ko` copy, so its initramfs was left
unchanged. Confirm the bootloader will select the kernel you just built for;
do not silently change its default or regenerate its configuration.

## 8. Activate later and verify on hardware

**There is intentionally no reboot or module-unload command in this guide.**
The operator decides when to stop streams and reboot during maintenance.

After that approved reboot, use:

```bash
uname -r
printf 'Loaded: '; cat /sys/module/stid135/srcversion
printf 'On disk: '; modinfo -F srcversion stid135
modinfo -n stid135
journalctl -k -b --no-pager | grep -Ei 'stid135|tbsecp3|unknown symbol|verification|signature'
```

Compare the loaded version with the staged build's recorded `srcversion`.
`modinfo` alone normally reads the file **on disk**, not the code already
loaded in memory. Missing `/sys/module/stid135` means it is not loaded; it
does not mean the fix is active. Diagnose load/signature/dependency errors
instead of forcing a module into the kernel.

Recheck adapter numbering/model/RF mapping after boot. To validate the fix:

1. Keep one or more sibling demodulators receiving stable streams on the same
   physical STiD135 RF input. Record lock, BER/UNC, packet arrival gaps, and
   MPEG-TS continuity-counter errors, not just a slowly refreshed UI meter.
2. Open/tune, restart, close/reopen, and stop another sibling. Repeat while
   monitoring the original streams. Use unique, correct Unicable user bands.
3. Check surviving siblings after one frontend stops; test independent RFs
   separately. Test last-user sleep/restart when powerdown is enabled.
4. Compare against the baseline and inspect kernel errors. Separate shared
   RF reset effects from external voltage/tone/DiSEqC command collisions.

The target is elimination of **redundant shared-RF initialization resets**,
not a guarantee of zero discontinuities from every source.

### Recorded validation status at publication

- Native Linux GCC 13: 24/24 source-executing cases passed; kernel 6.8.12
  module compilation passed with no new warning messages versus the base.
- Local sanitizer runs: 24/24 ASan/UBSan cases and 3/3 focused TSan cases passed.
- Targeted ABI and signing checks passed. The signed fixed module was installed
  on disk; no reboot or module reload was performed.
- On that build, original loaded `srcversion`: `D464F617AF5C487BCE79E57`;
  fixed on-disk `srcversion`: `188CD71F850BBDD97D35412`. These identify that
  build only; compiler/configuration changes can alter generated identifiers.
- Tuning/scan/restart tests before activation obtained lock and 28 services,
  but used the **old loaded driver**. They do not validate the fix on hardware.
  Post-activation sibling continuity remains an outstanding test.

## 9. Rollback

Before activation, restoring disk files needs no DVB reload. After activation,
restoring disk files still does not replace the code in memory; schedule a
separate operator-approved maintenance reboot to load the old driver.

Set `BACKUP` to the retained directory, restore the saved kernel/path variables,
and verify that the backup belongs to this exact installation before writing:

```bash
set -euo pipefail
BACKUP=/var/backups/stid135-before-fix.REPLACE_ME
KVER="$(sudo cat "$BACKUP/kernel.txt")"
MODROOT="$(readlink -f "/lib/modules/$KVER")"
MODFILE="$(sudo cat "$BACKUP/module-path.txt")"
case "$MODFILE" in
  "$MODROOT"/*.ko) ;;
  *) echo 'Stop: unexpected restore path.' >&2; exit 1 ;;
esac
sudo test -s "$BACKUP/stid135.ko"
RESTORE="$(sudo mktemp "${MODFILE}.restore.XXXXXXXX")"
sudo cp -a "$BACKUP/stid135.ko" "$RESTORE"
sudo cmp -- "$BACKUP/stid135.ko" "$RESTORE"
sudo mv -fT -- "$RESTORE" "$MODFILE"
sudo cmp -- "$BACKUP/stid135.ko" "$MODFILE"
sudo depmod -a "$KVER"
```

If `depmod` itself fails during an **immediate rollback with no intervening
package/driver changes**, restore the exact backed-up `module-metadata/`
files to `MODROOT` as well. Inventory and remove only generated `modules.*`
files introduced by the failed operation that were absent from that snapshot;
do not delete a whole module directory. Verify the restored bytes and stop
for operator repair if recovery is incomplete. Do not keep retrying `depmod`
while assuming the old indexes are intact, and do not reboot with partial
indexes. Historical index snapshots must not overwrite later package changes.

If an initramfs image was changed, restore its matching backup or rebuild it
from the restored module and verify the result before the next reboot. Keep
both module and image backups until post-activation testing succeeds.

## 10. Updates and maintenance

This branch does not provide a DKMS package or automatic rebuild/update hook.
A kernel or TBS-stack update can replace this file or require a new compatible
build. Retain the pinned source, prepared headers, build log, ABI/signing
evidence, module hashes, and rollback artifacts. Revalidate rather than copying
a `.ko` between kernels or blindly replacing newer upstream source with this
older base. Never publish private signing keys or production configuration.
