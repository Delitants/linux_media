#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile real STiD135 lifecycle callbacks against hardware-free C stubs."""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile


def code_mask(source):
    """Hide comments/literals without moving offsets used for extraction."""
    pattern = r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    return re.sub(pattern, lambda m: re.sub(r"[^\n]", " ", m[0]),
                  source, flags=re.S)


def block(source, pattern, label, semicolon=False):
    masked = code_mask(source)
    matches = list(re.finditer(pattern, masked, re.M))
    if len(matches) != 1:
        raise ValueError(f"{label}: expected one definition, found {len(matches)}")
    start = matches[0].start()
    opening = masked.index("{", start)
    depth = 1
    end = opening + 1
    while depth and end < len(masked):
        depth += (masked[end] == "{") - (masked[end] == "}")
        end += 1
    if depth:
        raise ValueError(f"{label}: unterminated body")
    if semicolon:
        if masked[end:].lstrip()[:1] != ";":
            raise ValueError(f"{label}: missing terminating semicolon")
        end = masked.index(";", end) + 1
    return source[start:end]


def function(source, name):
    return block(source, rf"^[\w \t*]+\b{re.escape(name)}\s*"
                 r"\([^;{}]*\)\s*\{", name)


def structure(source, name):
    return block(source, rf"^struct\s+{re.escape(name)}\s*\{{",
                 name, semicolon=True)


def extract(source_dir):
    frontend = (source_dir / "stid135-fe.c").read_text()
    lowlevel = (source_dir / "stid135_drv.c").read_text()
    chip = (source_dir / "chip.c").read_text()
    roots = ["stid135_probe", "stid135_init", "stid135_sleep", "stid135_release",
             "stid135_attach", "match_base", "stid135_set_voltage", "stid135_set_tone"]
    functions = {}
    pending = roots[:]
    while pending:
        name = pending.pop()
        if name in functions:
            continue
        body = function(frontend, name)
        functions[name] = body
        # New lifecycle helpers must be compiled too, never replaced by stubs.
        pending.extend(re.findall(r"\b(stid135_\w+)\s*\(", code_mask(body)))
    for name in ("fe_stid135_tuner_enable", "FE_STiD135_TunerStandby",
                 "fe_stid135_set_rfmux_path"):
        functions[name] = function(lowlevel, name)
    for name in ("ChipSetField", "ChipResetError", "ChipGetFieldMask",
                 "ChipGetFieldSign", "ChipGetFieldPosition", "ChipGetFieldBits"):
        functions[name] = function(chip, name)
    structs = [structure(frontend, "stv_base"), structure(frontend, "stv"),
               structure((source_dir / "stid135.h").read_text(), "stid135_cfg")]
    prototypes = [body[:body.index("{")].rstrip() + ";"
                  for body in functions.values()]
    return "\n\n".join(structs + prototypes + list(functions.values())) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path,
                        default=Path(__file__).resolve().parent.parent)
    parser.add_argument("cases", nargs="*", help="run only named cases")
    args = parser.parse_args()
    tests_dir = Path(__file__).resolve().parent
    source_dir = args.source_dir.resolve()
    try:
        extracted = extract(source_dir)
    except (OSError, ValueError) as error:
        parser.exit(2, f"Extraction failed: {error}\n")
    with tempfile.TemporaryDirectory(prefix=".shared-rf-", dir=tests_dir) as temp:
        build = Path(temp)
        (build / "linux").mkdir()
        for name in ("kernel.h", "delay.h", "slab.h", "mm.h"):
            (build / "linux" / name).write_text("/* Host-test kernel shim. */\n")
        (build / "shared-rf-source.h").write_text(extracted)
        binary = build / "shared-rf"
        command = shlex.split(os.environ.get("CC", "cc"))
        command += ["-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-pthread"]
        command += shlex.split(os.environ.get("CFLAGS", ""))
        command += ["-I", str(build), "-I", str(source_dir),
                    str(tests_dir / "shared_rf.c"), "-o", str(binary)]
        print("COMPILE:", shlex.join(command), flush=True)
        subprocess.run(command, check=True)
        names = subprocess.check_output([binary, "--list"], text=True).split()
        selected = args.cases or names
        unknown = set(selected) - set(names)
        if unknown:
            parser.error(f"unknown cases: {sorted(unknown)}")
        failed = 0
        for name in selected:
            try:
                result = subprocess.run([binary, name], timeout=15)
                failed += result.returncode != 0
            except subprocess.TimeoutExpired:
                print(f"FAIL {name}: timeout (possible deadlock)", flush=True)
                failed += 1
        print(f"RESULT: {len(selected) - failed}/{len(selected)} passed", flush=True)
        return bool(failed)


if __name__ == "__main__":
    sys.exit(main())
