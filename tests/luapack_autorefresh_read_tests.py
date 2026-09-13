"""Run production disk capture, watcher, and deferred reads with engine adapters."""
from pathlib import Path
import argparse
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--compiler", default="c++")
parser.add_argument("--msvc", action="store_true")
parser.add_argument("--sanitize", action="store_true")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / "source/modules/gmoddatapack.cpp").read_text(encoding="utf-8")
start = source.index("static bool ReadLuaAutoRefreshSource(")
end = source.index("void HolyLib::GModDataPack::InstallLuaAutoRefreshDetour()", start)
methods = source[start:end]

# The adapters exercise the production queue. Also require its real lifecycle
# entry points to call the drain/reset, rather than only wiring the test driver.
think = source[source.index("void CGModDataPackModule::Think("):]
assert "DrainLuaAutoRefreshReads();" in think[:think.index("// Do not consume")]
for name in ("LuaShutdown", "LevelShutdown", "Shutdown"):
    body = source[source.index("void CGModDataPackModule::" + name + "("):]
    assert "g_pendingLuaAutoRefreshReads.clear();" in body[:body.index("\n}")]

template = (root / "tests/luapack_autorefresh_read_harness.cpp").read_text(encoding="utf-8")
assert template.count("// INSERT_PRODUCTION_METHODS") == 1
with tempfile.TemporaryDirectory(prefix="luapack-refresh-") as directory:
    directory = Path(directory)
    cpp = directory / "refresh.cpp"
    cpp.write_text(template.replace("// INSERT_PRODUCTION_METHODS", methods), encoding="utf-8")
    binary = directory / ("refresh.exe" if args.msvc else "refresh")
    if args.msvc:
        command = [args.compiler, "/nologo", "/EHsc", "/std:c++17", "/W4",
                   "/I" + str(root / "source"), str(cpp), "/Fe:" + str(binary)]
    else:
        command = [args.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                   "-pedantic", "-pthread", "-I" + str(root / "source"),
                   str(cpp), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    subprocess.run(command, cwd=directory, check=True)
    subprocess.run([str(binary)], cwd=directory, check=True, timeout=15)
