"""Run upstream guard regressions against the actual production function bodies.

The adapters exercise memory bounds and callback contracts without loading GMod;
they do not qualify engine layouts or signatures. --source-ref permits checking
that a regression fails against a previous Git revision.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--compiler", default="c++")
parser.add_argument("--sanitize", action="store_true")
parser.add_argument("--source-ref")
parser.add_argument("--case", choices=("bitbuf", "voice", "metadata", "varargs", "networking", "async-read", "file-descriptor"))
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]


def read(name):
    if args.source_ref:
        return subprocess.check_output(["git", "show", args.source_ref + ":" + name], cwd=root, text=True)
    return (root / name).read_text(encoding="utf-8")


def extract(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    # These selected bodies have balanced braces inside their comments/strings.
    for position in range(opening, len(source)):
        if source[position] == "{":
            depth += 1
        elif source[position] == "}":
            depth -= 1
            if depth == 0:
                return source[start:position + 1]
    raise AssertionError("Unterminated production function: " + signature)


bitbuf = read("source/modules/bitbuf.cpp")
voice = read("source/modules/voicechat.cpp")
interface = read("source/lua/CLuaInterface.cpp")
filesystem = read("source/modules/filesystem.cpp")
program = (root / "tests/upstream_runtime_guard_harness.cpp").read_text(encoding="utf-8")
insertions = {
    "BITBUF": bitbuf[bitbuf.index("static constexpr int MAX_BUFFER_SIZE"):bitbuf.index("LUA_FUNCTION_STATIC(bitbuf_CopyReadBuffer)")] + "\n".join(
        extract(bitbuf, "LUA_FUNCTION_STATIC(" + name + ")") for name in
        ("bitbuf_CreateReadBuffer", "bitbuf_CreateWriteBuffer", "bitbuf_CreateStackWriteBuffer")),
    "VOICE": (extract(voice, "static uint16_t ClampVoiceDataLength(") if "static uint16_t ClampVoiceDataLength(" in voice else "") + "\n" + "\n".join([
        extract(voice, "LUA_JIT_WRAPPED_3(VoiceData_SetData,"),
        extract(voice, "LUA_JIT_RAW_2(VoiceData_SetData_NoLength,"),
        extract(voice, "LUA_FUNCTION_STATIC(voicechat_CreateVoiceData)"),
        extract(voice, "struct WavAudioFile") + ";",
    ]),
    "LUA_INTERFACE": "\n".join(extract(interface, signature) for signature in (
        "const char* CLuaInterface::GetTypeName(",
        "void CLuaInterface::CreateMetaTableType(",
        "bool CLuaInterface::PushMetaTable(",
        "void CLuaInterface::ErrorNoHalt(",
        "void CLuaInterface::ErrorFromLua(",
    )),
    "FAST_PCALL": extract(read("source/lua.h"), "inline bool FastPCall("),
    "SYS_ERROR": extract(read("source/util.cpp"), "DLL_EXPORT void hook_Sys_Error_Internal_ignore_this_function_in_crashes_check_your_logs_("),
    "ENTITY_REMOVED": extract(read("source/modules/networking.cpp"), "void EntityRemoved(CBaseEntity* pEntity, edict_t* pEdict)"),
    "ASYNC_READ": extract(filesystem, "struct IAsyncFile") + ";\nstd::vector<IAsyncFile*> asyncCallback;\n" + extract(filesystem, "void AsyncCallback("),
    "FILE_DESCRIPTOR": extract(read("source/modules/crashhandler.cpp"), "class ScopedFileDescriptor") + ";",
}
for name, implementation in insertions.items():
    marker = "// INSERT_" + name
    assert program.count(marker) == 1, marker
    program = program.replace(marker, implementation)

with tempfile.TemporaryDirectory(prefix="holylib-runtime-guards-") as temp:
    cpp, binary = Path(temp) / "test.cpp", Path(temp) / "test"
    cpp.write_text(program, encoding="utf-8")
    command = [args.compiler, "-std=c++17", "-Wall", "-Wextra", "-pedantic", str(cpp), "-o", str(binary)]
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
    subprocess.run(command, check=True)
    cases = [args.case] if args.case else ["bitbuf", "voice", "metadata", "varargs", "networking", "async-read", "file-descriptor"]
    for case in cases:
        subprocess.run([str(binary), case], check=True, timeout=20)
