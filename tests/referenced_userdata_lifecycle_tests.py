"""Exercise actual production lifecycle methods with isolated engine adapters."""
from pathlib import Path
import argparse
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--compiler", default="c++")
parser.add_argument("--msvc", action="store_true")
parser.add_argument("--util-source", type=Path)
parser.add_argument("--stringtable-source", type=Path)
parser.add_argument("--work-dir", type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]


def extract(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    # These bounded methods contain no braces inside strings or comments.
    for position in range(opening, len(source)):
        if source[position] == "{":
            depth += 1
        elif source[position] == "}":
            depth -= 1
            if depth == 0:
                return source[start:position + 1]
    raise AssertionError("Unterminated production method: " + signature)


util = (args.util_source or root / "source/util.cpp").read_text(encoding="utf-8")
stringtable = (args.stringtable_source or root / "source/modules/stringtable.cpp").read_text(encoding="utf-8")
methods = "\n\n".join([
    extract(util, "void ReferencedLuaUserData::ForceGlobalRelease(void* pData)"),
    extract(stringtable, "LUA_FUNCTION_STATIC(stringtable_RemoveTable)"),
])
template = (root / "tests/referenced_userdata_lifecycle_harness.cpp").read_text(encoding="utf-8")
assert template.count("// INSERT_PRODUCTION_METHODS") == 1
if args.work_dir:
    args.work_dir.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="userdata-lifecycle-", dir=args.work_dir) as directory:
    directory = Path(directory)
    cpp = directory / "lifecycle.cpp"
    cpp.write_text(template.replace("// INSERT_PRODUCTION_METHODS", methods), encoding="utf-8")
    binary = directory / ("lifecycle.exe" if args.msvc else "lifecycle")
    if args.msvc:
        command = [args.compiler, "/nologo", "/EHsc", "/std:c++17", "/W4", str(cpp), "/Fe:" + str(binary)]
    else:
        command = [args.compiler, "-std=c++17", "-Wall", "-Wextra", "-pedantic", str(cpp), "-o", str(binary)]
    subprocess.run(command, cwd=directory, check=True)
    subprocess.run([str(binary)], cwd=directory, check=True, timeout=15)
