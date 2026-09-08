# STiD135 shared-RF regression tests

For the patch explanation, source versions, kernel compilation, installation,
activation checks, and rollback, see the [latest-stack guide](../LATEST-STACK.md)
and the [historical fix explanation](../BUILDING-SHARED-RF.md).

Run from the source tree root with Python 3, a C11 compiler and pthreads:

```sh
python3 drivers/media/dvb-frontends/stid135/tests/shared_rf.py
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
