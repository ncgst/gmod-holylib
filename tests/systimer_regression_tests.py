"""Run the production systimer module with a deterministic clock and Lua adapter.

Only engine/Lua dependencies and the wall clock are substituted. Timer storage,
all public timer functions, dispatch, and shutdown come from systimer.cpp.
Use --source to check a baseline copy and --case to isolate a regression.
"""

import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--case", action="append", default=[])
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = (args.source or root / "source/modules/systimer.cpp").read_text()
    source = re.sub(r'^#include "[^\n]+"\n', "", source, flags=re.M)
    source, replacements = re.subn(
        r"static double GetTime\(\)\s*\{[^{}]*\}",
        "static double GetTime() { return TestClock::microseconds; }",
        source,
        count=1,
    )
    if replacements != 1:
        raise RuntimeError("Could not substitute the production wall clock")

    with tempfile.TemporaryDirectory(prefix="systimer-regressions-") as temp:
        directory = Path(temp)
        (directory / "systimer_production.inc").write_text(source)
        executable = directory / "systimer_tests"
        subprocess.run([
            "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-Wno-unused-parameter", "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g",
            "-I", str(directory), str(root / "tests/systimer_harness.cpp"),
            "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable), *args.case], check=True, timeout=30)


if __name__ == "__main__":
    main()
