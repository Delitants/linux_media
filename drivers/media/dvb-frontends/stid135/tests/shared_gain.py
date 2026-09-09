#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute the real search/gain policy with deterministic register/I2C stubs."""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from shared_rf import code_mask, function


def extract(root):
    source = (root / "stid135_drv.c").read_text()
    # Only unrelated signal-processing routines are stubbed. The complete search,
    # acquisition, lock-wait and gain paths (including new helpers) are executed.
    stubs = {
        "fe_stid135_set_reg_init_values": "return setup_error;",
        "fe_stid135_set_reg_values_wb": "return FE_LLA_NO_ERROR;",
        "FE_STiD135_TrackingOptimization": "return tracking_error;",
        "fe_stid135_reset_obs_registers": "return FE_LLA_NO_ERROR;",
        "FE_STiD135_GetLockTimeout": "*DemodTimeout = 10; *FecTimeout = 10;",
        "fe_stid135_set_symbol_rate": "return FE_LLA_NO_ERROR;",
        "fe_stid135_set_carrier_frequency_init": "return FE_LLA_NO_ERROR;",
        "FE_STiD135_SetSearchStandard": "return FE_LLA_NO_ERROR;",
        "FE_STiD135_StartSearch": "started[Demod - 1] = true; return FE_LLA_NO_ERROR;",
        "FE_STiD135_BlindSearchAlgo": "return FE_LLA_NOT_SUPPORTED;",
        "FE_STiD135_GetSignalParams": """
            pParams->demod_results[Demod-1].standard = FE_SAT_DVBS2_STANDARD;
            *range_p = FE_SAT_RANGEOK; return FE_LLA_NO_ERROR;
        """,
        "fe_stid135_manage_matype_info": "return FE_LLA_NO_ERROR;",
        "FE_STiD135_WaitForLock": "*lock_p = true; return FE_LLA_NO_ERROR;",
    }
    functions = {}
    pending = ["fe_stid135_search"]
    while pending:
        name = pending.pop()
        if name == "fe_stid135_manage_matype_info_raw_bbframe":
            continue  # This call is in the acquisition routine's disabled #else.
        if name in functions:
            continue
        body = function(source, name)
        if name in stubs:
            body = body[:body.index("{")] + "{\n" + stubs[name] + "\n}"
        else:
            # Exclude disabled C blocks when discovering callees.
            scanned = re.sub(r"#(?:ifdef|ifndef)\s+(?:USER2|ATB).*?#endif", "", body, flags=re.S)
            pending.extend(re.findall(r"\b((?:fe_stid135_|FE_STiD135_)\w+)\s*\(", code_mask(scanned)))
        functions[name] = body
    oxford = (root / "oxford_anafe_func.c").read_text(encoding="latin-1")
    if "fe_stid135_manage_LNF_IP3" in functions:
        for name in ("Oxford_GetVGLNAgainMode", "Oxford_SetVGLNAgainMode"):
            functions[name] = function(oxford, name)
    chip = (root / "chip.c").read_text()
    for name in ("ChipGetField", "ChipSetField", "ChipGetFieldMask", "ChipGetFieldSign",
                 "ChipGetFieldPosition", "ChipGetFieldBits"):
        functions[name] = function(chip, name)
    constants = re.findall(r"^#define (?:DmdLock_TIMEOUT_LIMIT|LNF_IP3_SWITCH_\w+|MODE_LNF|MODE_IP3)\s+[^\n]+", source, re.M)
    arrays = re.findall(r"^u32 \w+\[\d*\] = [^;]+;", (root / "stid135_init.c").read_text(), re.M)
    prototypes = [b[:b.index("{")].rstrip() + ";" for b in functions.values()]
    present = "#define GAIN_POLICY_PRESENT\n" if "fe_stid135_manage_shared_gain" in functions else ""
    return present + "\n\n".join(constants + arrays + prototypes + list(functions.values()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("cases", nargs="*")
    args = parser.parse_args()
    tests = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix=".shared-gain-", dir=tests) as tmp:
        build = Path(tmp)
        (build / "linux").mkdir()
        for name in ("kernel.h", "delay.h", "slab.h", "mm.h"):
            (build / "linux" / name).write_text("/* Host-test kernel shim. */\n")
        (build / "shared-gain-source.h").write_text(extract(args.source_dir))
        binary = build / "shared-gain"
        command = shlex.split(os.environ.get("CC", "cc"))
        command += ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter"]
        # The unpatched Oxford getter deliberately drops its error accumulator.
        if "GAIN_POLICY_PRESENT" not in (build / "shared-gain-source.h").read_text():
            command += ["-Wno-unused-but-set-variable"]
        command += shlex.split(os.environ.get("CFLAGS", ""))
        command += ["-I", str(build), "-I", str(args.source_dir), str(tests / "shared_gain.c"), "-o", str(binary)]
        print("COMPILE:", shlex.join(command), flush=True)
        subprocess.run(command, check=True)
        names = subprocess.check_output([binary, "--list"], text=True).split()
        selected = args.cases or names
        if set(selected) - set(names):
            parser.error("unknown case")
        failed = 0
        for name in selected:
            result = subprocess.run([binary, name], timeout=15)
            failed += result.returncode != 0
        print(f"RESULT: {len(selected) - failed}/{len(selected)} passed", flush=True)
        return bool(failed)


if __name__ == "__main__":
    sys.exit(main())
