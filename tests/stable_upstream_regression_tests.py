"""Exercise backported production functions with deterministic engine adapters.

Use --source-root with an older checkout to reproduce the fixed failures. These
tests cover C++ control flow and memory safety; they do not replace GMod runtime
qualification of the engine ABI.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile


def extract(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    # The selected functions have no braces inside strings or comments.
    for position in range(opening, len(source)):
        if source[position] == "{":
            depth += 1
        elif source[position] == "}":
            depth -= 1
            if depth == 0:
                return source[start:position + 1]
    raise AssertionError("Unterminated production function: " + signature)


parser = argparse.ArgumentParser()
parser.add_argument("--source-root", type=Path)
parser.add_argument("--compiler", default="c++")
parser.add_argument("--no-sanitize", action="store_true")
parser.add_argument("--case", choices=["player", "transmit", "stringtable", "callbacks", "shutdown"])
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source_root = args.source_root or root
fragments = {
    "// PLAYER_FUNCTION": [("source/util.cpp", "CBasePlayer* Util::Get_Player(")],
    "// TRANSMIT_FUNCTION": [("source/modules/networking.cpp", "static void CleanupSetPreventTransmit(")],
    "// STRINGTABLE_FUNCTION": [("source/modules/stringtable.cpp", "LUA_FUNCTION_STATIC(INetworkStringTable_SetStringUserData)")],
    "// CALLBACK_FUNCTION": [("source/modules/util.cpp", "void CUtilModule::LuaThink(")],
    "// SHUTDOWN_FUNCTIONS": [
        ("source/lua.cpp", "void Lua::Shutdown()"),
        ("source/lua.cpp", "void Lua::FinalShutdown()"),
        ("source/lua.cpp", "void Lua::ManualShutdown()"),
    ],
}
program = (root / "tests/stable_upstream_regression_harness.cpp").read_text(encoding="utf-8")
for marker, functions in fragments.items():
    assert program.count(marker) == 1
    bodies = [extract((source_root / path).read_text(encoding="utf-8"), signature)
              for path, signature in functions]
    program = program.replace(marker, "\n\n".join(bodies))

failed = []
with tempfile.TemporaryDirectory(prefix="stable-upstream-") as directory:
    cpp, binary = Path(directory) / "regression.cpp", Path(directory) / "regression"
    cpp.write_text(program, encoding="utf-8")
    command = [args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-g"]
    if not args.no_sanitize:
        command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
    subprocess.run(command + [str(cpp), "-o", str(binary)], check=True)
    for case in ([args.case] if args.case else ["player", "transmit", "stringtable", "callbacks", "shutdown"]):
        result = subprocess.run([str(binary), case], timeout=20)
        print(f"{case}: {'PASS' if result.returncode == 0 else 'FAIL'}", flush=True)
        if result.returncode:
            failed.append(case)
if failed:
    raise SystemExit("Failed regression cases: " + ", ".join(failed))
