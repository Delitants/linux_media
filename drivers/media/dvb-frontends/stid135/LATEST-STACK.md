# Latest TBS stack with the STiD135 shared-RF fix

Upstream checked: **2026-09-08 UTC**. Linux kernel upgrades, firmware changes,
driver reloads, and reboots are separate operator decisions.

## Versions and scope

| Component | Pinned revision |
| --- | --- |
| Official TBS `linux_media/latest` | [`2ce787de9f6e81b4294692a45de15596ae2e1322`](https://github.com/tbsdtv/linux_media/commit/2ce787de9f6e81b4294692a45de15596ae2e1322), 2026-07-24 |
| Latest plus the shared-RF fix | [`17df6d129ab2b718be2c828b462eccd65971905f`](https://github.com/Delitants/linux_media/commit/17df6d129ab2b718be2c828b462eccd65971905f) |
| Official `media_build/latest` | [`ff326ab69e786d447a0e0e967cb45acb5d5ca9ff`](https://github.com/tbsdtv/media_build/commit/ff326ab69e786d447a0e0e967cb45acb5d5ca9ff) |
| Tested native kernel/compiler | Linux 6.8.12, x86_64, GCC 13 |

The merge retains all 16 upstream commits since the original base without
rewriting published history. Documentation commits may follow the code pin.
These are dated versions, not an automatic future-upstream tracking service.

The fix prevents redundant RF clock/FIFO initialization when another STiD135
frontend already uses the same physical input. First-use initialization,
last-user standby, and error/retry handling remain. See the
[technical explanation](BUILDING-SHARED-RF.md#2-what-the-fix-does) and
[regression tests](tests/README.md). It does not arbitrate shared-coax SEC or
Unicable commands, prevent duplicate user-band collisions, or fix every TBS
model. Hardware continuity still requires testing after activation.

## Build the matching module set

Latest upstream changes shared DVB FEC/APSK definitions and bridge/frontend
drivers. Rebuilding only `stid135.ko` with old headers is not a validated
upgrade. Shared enum changes can alter symbol-version CRCs even when structure
sizes remain the same.

The preserved deployment configuration builds 583 modules. Its installed TBS
`updates/extra` subtree contains 582: 577 under `media`, plus five companions
under `misc` and `staging/media`. Treat these as one matched set. The separately
built `snd-bt87x.ko` resolves to the kernel sound directory and is intentionally
not replaced. Inventory actual paths on other machines instead of assuming
these counts or silently discarding missing modules.

## Userspace compatibility

Existing AstraX mappings for DVB-S2, QPSK, 8PSK, 16APSK, 32APSK, FEC-auto,
and its supported explicit FEC selections remain unchanged. No application
rebuild/restart is required solely for those values.

**Extended TBS enums are not backward-compatible.** Examples:

| Selection | Old value | Latest value |
| --- | --- | --- |
| `FEC_4_15` | 17 | 30 |
| `FEC_8_15` | 19 | 17 |
| `APSK_64` | 17 | 19 |
| `APSK_16_L` (formerly `APSK_16L`) | 21 | 17 |
| `QAM_512` | 14 | 21 |

Older external programs using extended literals can silently select a different
mode. Validate their mappings against the matching header. Both TBS versions
advertise DVB API 5.12, so that version alone cannot identify this ABI. Do not
switch userspace to new extended values while old modules remain loaded.

## Reproduce the isolated build

Prerequisites: Git, Python 3, GCC, GNU make/binutils, Perl, patchutils (`lsdiff`),
normal kernel-build dependencies, and the prepared build tree for the exact
target kernel. Keep its real `Module.symvers`; `modules_prepare` alone is not
sufficient. See the [kernel build requirements](https://docs.kernel.org/kbuild/modules.html).

Use one Bash shell. Change `CURRENT_BUILD` to your existing prepared TBS build
directory. Preserve and review its local patches/configuration. The tested
adjustments retain 128 adapters in both generation paths and a custom-kernel
backport adjustment. These commands leave the original tree untouched.
The recipe expects the inspected build baseline below plus tracked local edits;
staged and unstaged edits are both captured. Stop if the baseline differs or
untracked source files exist, and account for those changes separately. Locally
committed adjustments need their own reviewed patch; a working-tree diff cannot
capture them. Ignored generated build outputs are recreated, not migrated.

```bash
set -euo pipefail
KVER="$(uname -r)"
CURRENT_BUILD=/path/to/existing/prepared/media_build
WORK="$(mktemp -d "$HOME/tbs-latest.XXXXXXXX")"
CODE_REV=17df6d129ab2b718be2c828b462eccd65971905f
BUILD_REV=ff326ab69e786d447a0e0e967cb45acb5d5ca9ff
BASELINE_BUILD_REV=bc02baf59046b02e3eb71653d8aa8d98e79dc4e1
test -s "/lib/modules/$KVER/build/Module.symvers"
test "$(git -C "$CURRENT_BUILD" rev-parse HEAD)" = "$BASELINE_BUILD_REV"
test -z "$(git -C "$CURRENT_BUILD" ls-files --others --exclude-standard)"
git -C "$CURRENT_BUILD" diff --binary HEAD -- > "$WORK/local-build-adjustments.patch"
cp -a "$CURRENT_BUILD/v4l/.config" "$WORK/original-v4l.config"

git clone --filter=blob:none --depth 1 --sparse --single-branch \
  --branch fix/stid135-shared-rf-init \
  https://github.com/Delitants/linux_media.git "$WORK/media"
git -C "$WORK/media" sparse-checkout set \
  drivers/media drivers/staging/media drivers/misc/altera-stapl include sound/pci
git -C "$WORK/media" fetch --depth 1 --filter=blob:none origin "$CODE_REV"
git -C "$WORK/media" checkout --detach "$CODE_REV"
test "$(git -C "$WORK/media" rev-parse HEAD)" = "$CODE_REV"

git clone --depth 1 --single-branch --branch latest \
  https://github.com/tbsdtv/media_build.git "$WORK/media_build"
git -C "$WORK/media_build" fetch --depth 1 origin "$BUILD_REV"
git -C "$WORK/media_build" checkout --detach "$BUILD_REV"
if test -s "$WORK/local-build-adjustments.patch"; then
  git -C "$WORK/media_build" apply --check "$WORK/local-build-adjustments.patch"
  git -C "$WORK/media_build" apply "$WORK/local-build-adjustments.patch"
fi

python3 "$WORK/media/drivers/media/dvb-frontends/stid135/tests/shared_rf.py"
make -C "$WORK/media_build/linux" dir DIR="$WORK/media"
make -C "$WORK/media_build/v4l" release VER="$KVER"
cp -a "$WORK/original-v4l.config" "$WORK/media_build/v4l/.config"
make -C "$WORK/media_build/v4l" prepare
# Regenerate compatibility definitions from the preserved module selection.
cp -a "$WORK/original-v4l.config" "$WORK/media_build/v4l/.config"
make -C "$WORK/media_build/v4l" prepare
cmp "$WORK/original-v4l.config" "$WORK/media_build/v4l/.config"
grep 'CONFIG_DVB_MAX_ADAPTERS' "$WORK/media_build/v4l/config-compat.h"

nice -n 10 ionice -c 2 -n 7 make -C "/lib/modules/$KVER/build" \
  M="$WORK/media_build/v4l" -j4 modules 2>&1 | tee "$WORK/build.log"
printf 'Build directory: %s\n' "$WORK"
```

The tested configuration prints `#define CONFIG_DVB_MAX_ADAPTERS 128`. Stop
on failures or unexpected configuration changes. The build has vendor warnings;
successful compilation is not a warning-free source audit. Using the explicit
kernel `modules` target avoids the project's default post-build module helper.

## Validation and signing

Before installation, match every replacement to its installed path. Verify
`vermagic`, imported symbol CRCs against the selected providers/kernel, and
affected non-replaced consumers. Empty/unsupported decoder output is not a
pass. This custom kernel needed an ELF-level compact-version-record decoder.

Sign final copies with an existing trusted key. The deployment uses SHA-512,
matching its kernel configuration, and verifies each CMS signature and its
exact unsigned payload. Do not strip signed modules or publish keys. See the
[kernel signing guide](https://docs.kernel.org/admin-guide/module-signing.html).

Run `depmod` and resolution checks against an isolated module root first,
without loading modules. Inspect the boot image for copies of all replacements;
if any exist, include a verified image rebuild and image rollback in the plan.

## Installation requirements without activation

Do **not** use upstream `make install` for the live-server procedure. Independently
review the installer and ABI gates and rehearse failure/rollback before deployment.
Verify a complete private candidate copy of the existing `updates/extra` subtree,
snapshot the top-level `modules.*` indexes, and use same-filesystem Linux
`renameat2(RENAME_EXCHANGE)` to exchange the entire directories atomically.
Retain the original directory as the rollback copy. A failed index update must
restore the old tree and exact indexes without relying on a second `depmod`.

This requires verified inventories/hashes, supported atomic exchange, no
symlinks, and no concurrent package/module-management changes. Two ordinary
renames or per-module copies are not the same transaction. Recovery must refuse
to overwrite intervening unrelated changes. Keep the installer, manifest,
review reports, and backups in the server's private deployment directory; this
environment-specific bundle is not a generic installer shipped by this repo.

The operator chooses the later maintenance reboot. Do not reload a shared DVB
stack under active adapters. When no replaced module is embedded in the boot
image, leave that image unchanged. Record loaded versions, boot ID, service
PIDs/start times, and image hash before/after a no-reload installation.

### Fresh, unoccupied test machine only

For a dedicated test installation where replacement of the complete media
stack is intended, the official build project's usual install command is:

```bash
sudo make -C "$WORK/media_build" install
```

This is **not** the live-server procedure. It removes/replaces a broad module
set, can install firmware/sound helpers, and does not provide the private
transaction's rollback guarantees. Back up affected trees/indexes and arrange
signing/boot-image handling first. It was not executed on the live server for
this upgrade. See the [official build project](https://github.com/tbsdtv/media_build).

## Activation and rollback verification

After an operator-approved maintenance reboot:

```bash
uname -r
printf 'Loaded STiD135: '; cat /sys/module/stid135/srcversion
printf 'On-disk STiD135: '; modinfo -F srcversion stid135
printf 'Loaded bridge: '; cat /sys/module/tbsecp3/srcversion
printf 'On-disk bridge: '; modinfo -F srcversion tbsecp3
journalctl -k -b --no-pager | grep -Ei 'stid135|tbsecp3|unknown symbol|verification|signature'
```

Compare loaded identifiers with the specific build's recorded values.
`modinfo` normally reads disk files, not already-loaded code. Recheck adapter
numbering/model/input mapping. Monitor packet gaps and MPEG-TS continuity errors
on sibling streams while opening, tuning, restarting, and stopping another
frontend sharing the RF. Signal strength or one successful scan is insufficient.

Rollback must restore the entire matched module set and indexes, not only
`stid135.ko`. Restoring files after activation still needs a separately scheduled
reboot to load old code. Retain matching boot-image backups where applicable.
The private transaction's automatic rollback is bound to the original boot and
process identities. After reboot or unrelated changes, plan a new validated
restore instead of bypassing those safety checks.
No DKMS or automatic update hook is provided; future upgrades need revalidation.
