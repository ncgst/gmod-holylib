"""Compile the actual production lifecycle methods with deterministic engine adapters."""
from pathlib import Path
import argparse
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--compiler", default="c++")
parser.add_argument("--msvc", action="store_true")
parser.add_argument("--source", type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (args.source or root / "source/modules/gmoddatapack.cpp").read_text(encoding="utf-8")

def extract(signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    # These four bounded functions have no braces inside strings/comments.
    for position in range(opening, len(source)):
        if source[position] == "{":
            depth += 1
        elif source[position] == "}":
            depth -= 1
            if depth == 0:
                return source[start:position + 1]
    raise AssertionError("Unterminated production function: " + signature)

request = extract("std::shared_ptr<const Bootil::AutoBuffer> ActiveRefreshPayload(")
request = request.replace(" ActiveRefreshPayload(", " LuaDataPack::ActiveRefreshPayload(", 1)
methods = "\n\n".join([
    request,
    extract("static SIMPLETHREAD_RETURNVALUE WorkerThread("),
    extract("void LuaDataPack::Shutdown()"),
    extract("void LuaDataPack::Initialize()"),
])
template = (root / "tests/luapack_worker_lifecycle_harness.cpp").read_text(encoding="utf-8")
assert template.count("// INSERT_PRODUCTION_METHODS") == 1
with tempfile.TemporaryDirectory(prefix="luapack-worker-") as directory:
    directory = Path(directory)
    cpp = directory / "worker.cpp"
    cpp.write_text(template.replace("// INSERT_PRODUCTION_METHODS", methods), encoding="utf-8")
    binary = directory / ("worker.exe" if args.msvc else "worker")
    if args.msvc:
        command = [args.compiler, "/nologo", "/EHsc", "/std:c++17", "/W4", str(cpp), "/Fe:" + str(binary)]
    else:
        command = [args.compiler, "-std=c++17", "-Wall", "-Wextra", "-pedantic", "-pthread", str(cpp), "-o", str(binary)]
    subprocess.run(command, cwd=directory, check=True)
    subprocess.run([str(binary)], cwd=directory, check=True, timeout=15)
