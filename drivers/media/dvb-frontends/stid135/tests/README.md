# STiD135 shared-RF regression tests

For the patch explanation, source versions, kernel compilation, installation,
activation checks, and rollback, see the [latest-stack guide](../LATEST-STACK.md)
and the [historical fix explanation](../BUILDING-SHARED-RF.md).

Run from the source tree root with Python 3, a C11 compiler and pthreads:

```sh
python3 drivers/media/dvb-frontends/stid135/tests/shared_rf.py
python3 drivers/media/dvb-frontends/stid135/tests/shared_gain.py
```

`CC` and `CFLAGS` are supported. Optional sanitizer checks:

```sh
CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -g' \
  python3 drivers/media/dvb-frontends/stid135/tests/shared_rf.py
CFLAGS='-fsanitize=thread -g' \
  python3 drivers/media/dvb-frontends/stid135/tests/shared_rf.py \
  concurrent_siblings concurrent_duplicate concurrent_init_sleep
```

The runner creates and removes a temporary build directory below `tests/`.
Pass case names to select tests, or `--source-dir /path/to/stid135` to exercise
another source version without changing the checkout containing the tests.

To reproduce Linux LP64's `uint64_t = unsigned long` on an LP64 host that
normally uses `unsigned long long`, run the same callbacks with this fixture:

```sh
CFLAGS='-include drivers/media/dvb-frontends/stid135/tests/lp64_stdint.h' \
  python3 drivers/media/dvb-frontends/stid135/tests/shared_rf.py
```

The harness normalizes both signed and unsigned 64-bit aliases only while
including the unmodified LLA headers, then restores the host type names.
This avoids conflicting typedefs without suppressing compiler diagnostics.

## What executes

The runner extracts the actual `stid135_init`, `stid135_sleep`,
`stid135_release`, probe/attach/RF-selection callbacks, their local helpers,
and the low-level tuner enable/standby and mux bodies. It also extracts
`ChipSetField`, its field helpers and `ChipResetError` from `chip.c`, retaining
the real software-error latch and I/O gate. It compiles the real
`stv_base`, `stv`, configuration and LLA structures; the latter come from
the unmodified driver headers. Missing or ambiguous definitions fail
extraction, and unresolved dependencies fail compilation. Callback logic is
not copied into the test or tested by checking source text.

Kernel interfaces, LLA probe setup, DiSEqC operations and register/Oxford I/O
are replaced with checked stubs. The actual enable body performs the clock
reset writes and 10/100 ms waits against fake hardware. The standby fake
models a partial powerdown even when it reports an I/O error. Runtime I/O
requires the owning thread to hold the shared mutex; probe retains its
existing, pre-publication initialization behavior.

The register stubs mirror the register-access latch reset and latch a failed
mux write. Sibling and duplicate init retries must perform fresh register
reads/writes without extra enable, clock reset, DiSEqC setup or standby.
Repeated failing I/O must still report failure, rather than hiding new errors.

The 24 cases cover first use despite multiple attachments, duplicate init
(including reopen without sleep), eight demods sharing one RF, four
independent RFs, partial/final/repeated/inactive sleep, one-shot release with
active or inactive ownership, board callbacks that suppress powerdown,
failure and retry at each init stage,
failed standby/cleanup, surviving sibling ownership, and multiswitch/RF
selection behavior. Three pthread cases hold the first callback in I/O
until a second callback has actually encountered the busy shared mutex.
Each case runs in a separate process with a deadlock timeout.

## RF state contract

- `rf_users` counts successfully initialized frontends, not attachments or
  open file descriptors. `rf_active` makes repeated init/sleep idempotent.
  Without a sleep callback, ownership persists until release.
- `rf_ready` records successful tuner and DiSEqC initialization independently
  of ownership. First use preserves the hardware FIFO/clock reset; later
  users only program their own demod mux.
- Sleep/release drops only that frontend's ownership. The last user enters
  standby unless the board callback suppresses powerdown, in which case RF
  readiness survives even with zero users.
- Any attempted standby invalidates readiness, including failed/partial
  standby. A later init must reinitialize. Failed init acquires no new owner
  and only attempts cleanup when the RF has no existing users.
- All these transitions and runtime RF operations hold `status_lock`.
  Init clears a prior software chip error even when a ready RF skips enable;
  errors from the new I/O attempt still propagate normally.
  Multiswitch probe initialization and voltage/tone RF selection are unchanged.

## Limits

This is not a kernel module build, hardware lock test, or proof of RF
continuity on a board. Most LLA I/O is stubbed, and the existing parent-driver
attachment/lifetime serialization is assumed; this does not redesign the
global base list. Board-specific voltage/power behavior and external changes
to hardware are not inferred by the readiness flag. Shared SEC voltage/tone,
DiSEqC commands and Unicable protocol arbitration remain separate limitations.

## Gain isolation tests

`shared_gain.py` executes the complete real `fe_stid135_search`,
`FE_STiD135_Algo`, `FE_STiD135_GetDemodLock`, RF-path getter, gain policy and
chip field-access bodies. Unrelated signal-processing routines and hardware
I/O are stubbed; actual register maps/constants and driver structures are used.
The 15 named cases contain threshold matrices across all four RF inputs and
1,344 reception combinations across requester/sibling pairs, gain directions
and live states, plus other-RF independence,
stale-cache rejection, recovering DVB-S reception, checked RF bounds, aborts,
I2C failures, error/timeout cleanup and diagnostic records.

The lock-wait stub deterministically schedules a second real search during
the first search's **actual mutex-release window**. It verifies same-RF
protection, different-RF independence, rejection of duplicate-demod searches
and survival/cleanup of the first reservation. This is a controlled interleaving,
not a pthread stress test or a claim to model all kernel scheduling behavior.
The gain byte preserves the other RF gain and differential-input bits. Errors
at gain/routing/AGC/status reads must result in no gain write. Acquisition errors
must propagate rather than being overwritten with apparent success.

One RF-bounds case calls the actual policy directly; invalid RF routing cannot
be encoded in the hardware's two-bit selector. Diagnostic tests validate the
decision record, not the kernel's logging/rate-limiter implementation.

`CC`, `CFLAGS`, individual case names and `--source-dir` also work here. On the
unpatched source the runner additionally tolerates the existing Oxford getter's
unused error accumulator; patched policy tests compile without that waiver.
The initial unpatched run failed the live-sibling, overlapping-acquisition and
read/error-propagation assertions while exclusive hysteresis tests passed.

```sh
CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -g' \
  python3 drivers/media/dvb-frontends/stid135/tests/shared_gain.py
```

See [the gain-fix guide](../SHARED-RF-GAIN.md) for hardware validation and the
intentional sensitivity/headroom trade-off. No DVB device is opened by tests.
